/* np_goreplay -- per-GO oracle for a NEWPORT_DRAWCAP capture.
 *
 *   np_goreplay CAPTURE.zst [--max N] [--quiet]
 *
 * For every GO record: load the recorded REX3 + RB2 state and the pre-image of
 * every framebuffer word the GO touched, run that one GO, and require exactly the
 * recorded result: the same set of touched words with the same new values, the
 * same pixel count, and the same REX3/RB2 state afterwards.  A mismatch means
 * either a behaviour change (new kernel vs the capturing build) or state missing
 * from the record.  Checksum records are counted only (a whole-stream replay is
 * np_replay's job). */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include "../interpret.hh"
#include "../sparse_mem.hh"
#include "../sgi_newport.hh"

static bool rd(FILE *f, void *p, size_t n) {
  return fread(p, 1, n, f) == n;
}

int main(int argc, char **argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s CAPTURE.zst [--max N] [--quiet]\n", argv[0]);
    return 1;
  }
  uint64_t max = ~0ull;
  bool quiet = false;
  for(int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    if(a == "--max" && i + 1 < argc) {
      max = strtoull(argv[++i], nullptr, 0);
    }
    else if(a == "--quiet") {
      quiet = true;
    }
  }
  const std::string cmd = std::string("zstd -dc '") + argv[1] + "'";
  FILE *f = popen(cmd.c_str(), "r");
  if(f == nullptr) {
    perror("popen");
    return 1;
  }
  sparse_mem *sm = new sparse_mem();
  state_t *s = new state_t(*sm);
  sgi_newport *np = new sgi_newport(s);
  s->np = np;
  const size_t rs = sgi_newport::rex_size();
  std::vector<uint8_t> before(rs), after(rs), got(rs);
  std::vector<sgi_newport::touch_t> want;
  static const char *ops[4] = { "noop", "read", "draw", "scr2scr" };
  static const char *adr[8] = { "span", "block", "iline", "fline", "aline", "adr5", "adr6", "adr7" };
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> by_kind;   /* kind -> {records, bad} */
  uint64_t ngo = 0, nbad = 0, nsum = 0, ntouch = 0;
  double sec = 0;
  for(;;) {
    uint32_t magic;
    uint16_t ver, kind;
    if(!rd(f, &magic, 4) || !rd(f, &ver, 2) || !rd(f, &kind, 2)) {
      break;
    }
    if(magic != 0x4f47504e || ver != 1) {
      fprintf(stderr, "bad record header %08x v%u\n", magic, ver);
      return 1;
    }
    if(kind == 2) {
      uint64_t v[4];
      rd(f, v, sizeof(v));
      nsum++;
      continue;
    }
    uint64_t icnt, pixels;
    uint32_t pc, rsz, n;
    uint8_t hdr[4];
    sgi_newport::rb_snap_t rb0, rb1, rbg;
    rd(f, &icnt, 8);
    rd(f, &pc, 4);
    rd(f, hdr, 4);
    rd(f, &pixels, 8);
    rd(f, &rsz, 4);
    if(rsz != rs) {
      fprintf(stderr, "capture rex_t size %u, this build %zu: rebuild against the capturing source\n", rsz, rs);
      return 1;
    }
    rd(f, before.data(), rs);
    rd(f, &rb0, sizeof(rb0));
    rd(f, &n, 4);
    want.resize(n);
    rd(f, want.data(), n * sizeof(sgi_newport::touch_t));
    rd(f, after.data(), rs);
    rd(f, &rb1, sizeof(rb1));
    if(ngo >= max) {
      continue;
    }
    ngo++;
    ntouch += n;
    /* inputs: state + pre-images */
    np->set_state(before.data(), rb0);
    for(const auto &t : want) {
      *np->fb_word(t.addr) = t.old_v;
    }
    const auto t0 = std::chrono::steady_clock::now();
    np->touch_begin();
    np->run_go();
    const std::vector<sgi_newport::touch_t> &gotw = np->touch_end();
    sec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    np->get_state(got.data(), &rbg);
    /* compare: touched words (as sets), pixels, post-state */
    std::string why;
    if(np->last_go_pixels() != pixels) {
      why += " pixels";
    }
    if(memcmp(got.data(), after.data(), rs) != 0) {
      why += " rex-state";
    }
    if(memcmp(&rbg, &rb1, sizeof(rbg)) != 0) {
      why += " rb-state";
    }
    if(gotw.size() != want.size()) {
      why += " touched-count";
    }
    else {
      std::vector<sgi_newport::touch_t> a = gotw, b = want;
      auto by_addr = [](const sgi_newport::touch_t &x, const sgi_newport::touch_t &y) { return x.addr < y.addr; };
      std::sort(a.begin(), a.end(), by_addr);
      std::sort(b.begin(), b.end(), by_addr);
      for(size_t i = 0; i < a.size(); i++) {
        if(a[i].addr != b[i].addr || a[i].new_v != b[i].new_v) {
          why += " fb-words";
          break;
        }
      }
    }
    const uint32_t k = hdr[1] | (hdr[2] << 2);
    by_kind[k].first++;
    if(!why.empty()) {
      by_kind[k].second++;
      if(nbad++ < 20 && !quiet) {
        fprintf(stderr, "MISMATCH GO #%llu icnt %llu pc %08x asid %u %s %s:%s\n", (unsigned long long)ngo,
                (unsigned long long)icnt, pc, hdr[0], ops[hdr[1] & 3], adr[hdr[2] & 7], why.c_str());
      }
    }
  }
  pclose(f);
  printf("%llu GO records, %llu framebuffer words, %llu checksum records; %llu mismatches; GO time %.3f s\n",
         (unsigned long long)ngo, (unsigned long long)ntouch, (unsigned long long)nsum, (unsigned long long)nbad, sec);
  for(const auto &kv : by_kind) {
    printf("  %-7s %-5s %10llu records  %llu bad\n", ops[kv.first & 3], adr[(kv.first >> 2) & 7],
           (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
  }
  return nbad ? 2 : 0;
}
