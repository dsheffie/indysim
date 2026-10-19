/* np_replay -- drive sgi_newport from a NEWPORT_TRACE file with no CPU.
 *
 *   np_replay TRACE [--lo ICNT] [--hi ICNT] [--every N] [--hashes FILE]
 *
 * Replays every CPU access (and MC-DMA-issued access) in order: writes with
 * their value, reads for their side effects (DCB auto-increment, STATUS ack,
 * host-port readback).  Accesses with icnt in [lo, hi) are timed; everything
 * before lo only rebuilds the board state.  Prints GOs/s and pixels/s overall
 * and per GO kind, and an FNV-1a hash of both framebuffers every N GOs and at
 * the end (also written to --hashes) so two builds can be compared bit-exact. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "../interpret.hh"
#include "../sparse_mem.hh"
#include "../sgi_newport.hh"

struct __attribute__((packed)) trace_rec_t {
  uint8_t type, size, rw, aux;
  uint32_t offset;
  uint64_t icnt, value;
};

static uint64_t fnv64(const uint32_t *p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
  for(size_t i = 0; i < n * 4; i++) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

static uint64_t fb_hash(sgi_newport *np) {
  const size_t n = sgi_newport::FB_W * sgi_newport::FB_H;
  return fnv64(np->fb_word(0x80000000u), n, fnv64(np->fb_word(0), n));
}

static uint64_t to_mem_order(uint64_t v, uint8_t sz) {
  switch(sz)
    {
    case 8: return __builtin_bswap64(v);
    case 4: return __builtin_bswap32((uint32_t)v);
    case 2: return __builtin_bswap16((uint16_t)v);
    default: return v & 0xff;
    }
}

int main(int argc, char **argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s TRACE [--lo ICNT] [--hi ICNT] [--every N] [--hashes FILE]\n", argv[0]);
    return 1;
  }
  uint64_t lo = 0, hi = ~0ull, every = 10000;
  const char *hashes = nullptr;
  for(int i = 2; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if(a == "--lo") {
      lo = strtoull(argv[i + 1], nullptr, 0);
    }
    else if(a == "--hi") {
      hi = strtoull(argv[i + 1], nullptr, 0);
    }
    else if(a == "--every") {
      every = strtoull(argv[i + 1], nullptr, 0);
    }
    else if(a == "--hashes") {
      hashes = argv[i + 1];
    }
  }
  FILE *f = fopen(argv[1], "rb");
  if(f == nullptr) {
    perror(argv[1]);
    return 1;
  }
  std::vector<trace_rec_t> recs;
  {
    trace_rec_t r;
    while(fread(&r, sizeof(r), 1, f) == 1) {
      if(r.icnt >= hi) {
        break;
      }
      recs.push_back(r);
    }
  }
  fclose(f);
  sparse_mem *sm = new sparse_mem();
  state_t *s = new state_t(*sm);
  sgi_newport *np = new sgi_newport(s);
  s->np = np;
  FILE *hf = hashes ? fopen(hashes, "w") : nullptr;

  static const char *ops[4] = { "noop", "read", "draw", "scr2scr" };
  static const char *adr[8] = { "span", "block", "iline", "fline", "aline", "adr5", "adr6", "adr7" };
  struct kind_t {
    uint64_t gos = 0, pixels = 0;
    double sec = 0;
  };
  std::map<uint32_t, kind_t> kinds;
  uint64_t ngo = 0, timed_acc = 0, timed_go = 0, timed_px = 0;
  /* least-squares fit of GO time against pixels: fixed cost per GO + cost per pixel */
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  double timed_sec = 0, go_sec = 0;
  for(size_t i = 0; i < recs.size(); i++) {
    const trace_rec_t &r = recs[i];
    if(r.type != 0) {
      continue;
    }
    s->icnt = r.icnt;
    const bool timed = r.icnt >= lo;
    /* the GO record (type 1) for a GO access follows it immediately */
    const bool is_go = (i + 1 < recs.size()) && recs[i + 1].type == 1 && recs[i + 1].icnt == r.icnt;
    const auto t0 = std::chrono::steady_clock::now();
    if(r.rw & 1) {
      np->write(r.offset, to_mem_order(r.value, r.size), r.size);
    }
    else {
      (void)np->read(r.offset, r.size);
    }
    if(timed) {
      const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      timed_sec += dt;
      timed_acc++;
      if(is_go) {
        const trace_rec_t &g = recs[i + 1];
        if(np->last_go_pixels() != g.value) {
          fprintf(stderr, "pixel count mismatch at icnt %llu: trace %llu replay %llu\n",
                  (unsigned long long)g.icnt, (unsigned long long)g.value,
                  (unsigned long long)np->last_go_pixels());
        }
        kind_t &k = kinds[g.aux];
        k.gos++;
        k.pixels += g.value;
        k.sec += dt;
        go_sec += dt;
        sx += (double)g.value;
        sy += dt;
        sxx += (double)g.value * (double)g.value;
        sxy += (double)g.value * dt;
        timed_go++;
        timed_px += g.value;
      }
    }
    if(is_go && (++ngo % every) == 0 && hf) {
      fprintf(hf, "%llu %016llx\n", (unsigned long long)ngo, (unsigned long long)fb_hash(np));
    }
  }
  const uint64_t final_hash = fb_hash(np);
  if(hf) {
    fprintf(hf, "final %llu %016llx\n", (unsigned long long)ngo, (unsigned long long)final_hash);
    fclose(hf);
  }
  printf("replayed %zu records, %llu GOs; final fb hash %016llx\n", recs.size(), (unsigned long long)ngo,
         (unsigned long long)final_hash);
  printf("timed window: %llu accesses in %.3f s (%.2f M accesses/s); GO accesses %.3f s\n",
         (unsigned long long)timed_acc, timed_sec, timed_acc / timed_sec / 1e6, go_sec);
  printf("  %llu GOs (%.3f M GOs/s), %llu pixels (%.1f M pixels/s)\n", (unsigned long long)timed_go,
         timed_go / go_sec / 1e6, (unsigned long long)timed_px, timed_px / go_sec / 1e6);
  if(timed_go > 1) {
    const double nn = (double)timed_go;
    const double b = (nn * sxy - sx * sy) / (nn * sxx - sx * sx), a = (sy - b * sx) / nn;
    printf("  fit: %.1f ns per GO + %.2f ns per pixel -> per-GO overhead %.0f%% of GO time, pixel work %.0f%%\n",
           a * 1e9, b * 1e9, 100.0 * a * nn / sy, 100.0 * b * sx / sy);
  }
  for(const auto &kv : kinds) {
    const kind_t &k = kv.second;
    printf("  %-7s %-5s GOs %9llu  pixels %11llu  %8.3f s  %7.3f M GOs/s  %8.1f M px/s  %6.1f ns/GO\n",
           ops[kv.first & 3], adr[(kv.first >> 2) & 7], (unsigned long long)k.gos, (unsigned long long)k.pixels,
           k.sec, k.gos / k.sec / 1e6, k.pixels / k.sec / 1e6, k.sec / k.gos * 1e9);
  }
  if(getenv("NP_AUXDUMP")) {   /* raw aux (CID/PUP/OLAY) planes, FB_W x FB_H u32 */
    FILE *af = fopen(getenv("NP_AUXDUMP"), "wb");
    fwrite(np->fb_word(0x80000000u), 4, sgi_newport::FB_W * sgi_newport::FB_H, af);
    fclose(af);
  }
  delete np;                  /* NEWPORT_PPM=<file>: the destructor dumps the final display */
  return 0;
}
