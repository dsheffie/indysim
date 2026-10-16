/* np_packplay -- replay pre-decoded per-GO packets (tools/np_pack) with no register
 * decode: apply the dirty decoded words, the RB2 state if present, run the GO.
 *
 *   np_packplay PACKETS [EXPECT]
 *
 * With EXPECT, checks every GO's post-state hash and pixel count, and the final
 * framebuffer hash (bit-exact against the register-path replay that made them).
 * Timing excludes file loading (packets are pre-loaded); with -DNP_PROF it also
 * splits out the GO commands and their pixel work. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <chrono>
#include "../interpret.hh"
#include "../sparse_mem.hh"
#include "../sgi_newport.hh"

static uint64_t fnv64(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
  for(size_t i = 0; i < n; i++) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

int main(int argc, char **argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s PACKETS [EXPECT]\n", argv[0]);
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if(f == nullptr) {
    perror(argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf((size_t)len);
  if(fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    fprintf(stderr, "short read\n");
    return 1;
  }
  fclose(f);
  sparse_mem *sm = new sparse_mem();
  state_t *s = new state_t(*sm);
  sgi_newport *np = new sgi_newport(s);
  s->np = np;
  const uint8_t *p = buf.data();
  uint32_t magic, W, ngo;
  memcpy(&magic, p, 4);
  memcpy(&W, p + 4, 4);
  memcpy(&ngo, p + 8, 4);
  p += 12;
  if(magic != 0x4b50504e || W * 4 != sgi_newport::rex_size()) {
    fprintf(stderr, "bad packet file (magic %08x, %u words vs %zu)\n", magic, W, sgi_newport::rex_size() / 4);
    return 1;
  }
  sgi_newport::rb_snap_t rb;
  np->set_state(p, *reinterpret_cast<const sgi_newport::rb_snap_t *>(p + W * 4));
  p += W * 4 + sizeof(rb);
  const size_t fbn = sgi_newport::FB_W * sgi_newport::FB_H;
  memcpy(np->fb_word(0), p, fbn * 4);
  p += fbn * 4;
  memcpy(np->fb_word(0x80000000u), p, fbn * 4);
  p += fbn * 4;
  const uint8_t *pk0 = p;
  const size_t bmb = (W + 7) / 8;
  std::vector<uint64_t> expect;
  if(argc > 2) {
    FILE *e = fopen(argv[2], "rb");
    uint64_t v;
    while(e && fread(&v, 8, 1, e) == 1) {
      expect.push_back(v);
    }
    if(e) {
      fclose(e);
    }
  }
  const bool verify = !expect.empty();
  uint32_t *rw = np->rex_words();
  std::vector<uint8_t> rexb(W * 4);
  uint64_t bad = 0, pixels = 0;
#ifdef NP_PROF
  g_np_prof_cmd = 0;
  g_np_prof_pix = 0;
#endif
  const auto t0 = std::chrono::steady_clock::now();
  for(uint32_t g = 0; g < ngo; g++) {
    const uint8_t flags = p[1];
    const uint8_t *bm = p + 2;
    const uint32_t *v = reinterpret_cast<const uint32_t *>(p + 2 + bmb);
    size_t nv = 0;
    for(size_t i = 0; i < bmb; i++) {
      uint32_t b = bm[i];
      while(b) {
        const int j = __builtin_ctz(b);
        b &= b - 1;
        rw[i * 8 + j] = v[nv++];
      }
    }
    p = reinterpret_cast<const uint8_t *>(v + nv);
    if(flags & 1) {
      memcpy(&rb, p, sizeof(rb));
      np->set_rb(rb);
      p += sizeof(rb);
    }
    np->run_go();
    pixels += np->last_go_pixels();
    if(verify) {
      np->get_state(rexb.data(), &rb);
      const uint64_t h = fnv64(&rb, sizeof(rb), fnv64(rexb.data(), rexb.size()));
      if(h != expect[2 * g] || np->last_go_pixels() != expect[2 * g + 1]) {
        if(bad++ < 10) {
          fprintf(stderr, "GO %u: post-state mismatch\n", g);
        }
      }
    }
  }
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("%u GOs from %.1f MB of packets (%.1f B/GO): %.3f s, %.1f ns/GO, %.2f M GOs/s, %.1f M pixels/s\n", ngo,
         (double)(p - pk0) / 1e6, (double)(p - pk0) / ngo, sec, sec * 1e9 / ngo, ngo / sec / 1e6, pixels / sec / 1e6);
#ifdef NP_PROF
  {
    /* calibrate ticks against this run's wall time is not possible here (no loop
     * ticks); report do_command and pixel shares of do_command time instead */
    const double cmd = (double)g_np_prof_cmd, pix = (double)g_np_prof_pix;
    printf("  of the GO commands' time: setup %.1f%%, pixel work %.1f%%\n", 100.0 * (cmd - pix) / cmd, 100.0 * pix / cmd);
  }
#endif
  if(verify) {
    const uint64_t fbh = fnv64(np->fb_word(0x80000000u), fbn * 4, fnv64(np->fb_word(0), fbn * 4));
    printf("verify: %llu post-state mismatches; final framebuffer %s\n", (unsigned long long)bad,
           fbh == expect.back() ? "identical" : "DIFFERS");
  }
  return 0;
}
