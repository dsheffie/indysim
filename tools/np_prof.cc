/* np_prof -- where does a Newport GO's time go?  Build with -DNP_PROF.
 *
 *   np_prof TRACE --lo ICNT --hi ICNT [--notime]
 *
 * Replays a NEWPORT_TRACE (pre-loaded into memory) like np_replay and, over
 * [lo, hi), splits the model's time into:
 *   (a) register access handling: every access, minus the GO commands it ran
 *   (b) per-GO setup: do_command minus pixel work
 *   (c) pixel work: row segments (span kernels / per-pixel), lines, scr2scr, readback
 *   (d) replay-loop overhead: the timed loop minus all model time
 * --notime runs the same loop with no per-access timers (the instrumentation
 * overhead is the difference in loop time; do_command/pixel ticks are compiled in
 * either way, see NP_PROF in sgi_newport.cc). */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include "../interpret.hh"
#include "../sparse_mem.hh"
#include "../sgi_newport.hh"

struct __attribute__((packed)) trace_rec_t {
  uint8_t type, size, rw, aux;
  uint32_t offset;
  uint64_t icnt, value;
};

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
    fprintf(stderr, "usage: %s TRACE --lo ICNT --hi ICNT [--notime]\n", argv[0]);
    return 1;
  }
  uint64_t lo = 0, hi = ~0ull;
  bool notime = false;
  for(int i = 2; i < argc; i++) {
    const std::string a = argv[i];
    if(a == "--lo" && i + 1 < argc) {
      lo = strtoull(argv[++i], nullptr, 0);
    }
    else if(a == "--hi" && i + 1 < argc) {
      hi = strtoull(argv[++i], nullptr, 0);
    }
    else if(a == "--notime") {
      notime = true;
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
    while(fread(&r, sizeof(r), 1, f) == 1 && r.icnt < hi) {
      recs.push_back(r);
    }
  }
  fclose(f);
  /* only accesses matter to the replay; split them into the warm-up prefix and the window */
  std::vector<trace_rec_t> pre, win;
  uint64_t gos = 0, pixels = 0;
  for(size_t i = 0; i < recs.size(); i++) {
    if(recs[i].type == 1) {
      if(recs[i].icnt >= lo) {
        gos++;
        pixels += recs[i].value;
      }
      continue;
    }
    if(recs[i].type != 0) {
      continue;
    }
    (recs[i].icnt < lo ? pre : win).push_back(recs[i]);
  }
  sparse_mem *sm = new sparse_mem();
  state_t *s = new state_t(*sm);
  sgi_newport *np = new sgi_newport(s);
  s->np = np;
  for(const trace_rec_t &r : pre) {
    s->icnt = r.icnt;
    if(r.rw & 1) {
      np->write(r.offset, to_mem_order(r.value, r.size), r.size);
    }
    else {
      (void)np->read(r.offset, r.size);
    }
  }
  uint64_t nwr = 0, nrd = 0;
  for(const trace_rec_t &r : win) {
    if(r.rw & 1) {
      nwr++;
    }
    else {
      nrd++;
    }
  }
  g_np_prof_cmd = 0;
  g_np_prof_pix = 0;
  uint64_t t_acc = 0;
  g_np_prof_rows = 0;
  /* fit of pixel ticks per GO against its pixel count: per-GO-call fixed + per pixel */
  double sx = 0, sy = 0, sxx = 0, sxy = 0, nfit = 0;
  /* pixel ticks by GO size: 0 px (fully masked), 1-32, 33-4095, >= 4096 (clears) */
  double bk_t[4] = {0, 0, 0, 0}, bk_n[4] = {0, 0, 0, 0}, bk_p[4] = {0, 0, 0, 0};
  const auto w0 = std::chrono::steady_clock::now();
  const uint64_t l0 = np_ticks();
  if(notime) {
    for(const trace_rec_t &r : win) {
      s->icnt = r.icnt;
      if(r.rw & 1) {
        np->write(r.offset, to_mem_order(r.value, r.size), r.size);
      }
      else {
        (void)np->read(r.offset, r.size);
      }
    }
  }
  else {
    for(const trace_rec_t &r : win) {
      s->icnt = r.icnt;
      const uint64_t pix0 = g_np_prof_pix;
      const uint64_t t0 = np_ticks();
      if(r.rw & 1) {
        np->write(r.offset, to_mem_order(r.value, r.size), r.size);
      }
      else {
        (void)np->read(r.offset, r.size);
      }
      t_acc += np_ticks() - t0;
      if(r.offset & 0x800) {                   /* a GO: one fit point */
        const double x = (double)np->last_go_pixels(), y = (double)(g_np_prof_pix - pix0);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        nfit += 1;
        const int bk = x == 0 ? 0 : (x <= 32 ? 1 : (x < 4096 ? 2 : 3));
        bk_t[bk] += y;
        bk_n[bk] += 1;
        bk_p[bk] += x;
      }
    }
  }
  const uint64_t loop = np_ticks() - l0;
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  const double ns = wall * 1e9 / (double)loop;   /* ns per tick, calibrated on this run */
  /* tick cost (one read), to judge the instrumentation */
  uint64_t tc0 = np_ticks(), sink = 0;
  for(int i = 0; i < 1000000; i++) {
    sink += np_ticks();
  }
  const double tick_ns = (double)(np_ticks() - tc0) * ns / 1e6;
  printf("window: %zu accesses (%llu writes, %llu reads), %llu GOs, %llu pixels; loop %.3f s; tick read %.1f ns%s\n",
         win.size(), (unsigned long long)nwr, (unsigned long long)nrd, (unsigned long long)gos,
         (unsigned long long)pixels, wall, tick_ns, notime ? " [--notime]" : "");
  const double cmd = g_np_prof_cmd * ns, pix = g_np_prof_pix * ns;
  if(!notime) {
    const double acc = t_acc * ns;
    const double a = acc - cmd, b = cmd - pix, d = wall * 1e9 - acc;
    printf("  (a) register access handling %8.3f s  %7.1f ns/GO  %6.1f ns per access\n", a / 1e9, a / gos,
           a / win.size());
    printf("  (b) per-GO setup             %8.3f s  %7.1f ns/GO\n", b / 1e9, b / gos);
    printf("  (c) pixel work               %8.3f s  %7.1f ns/GO  %6.2f ns/pixel\n", pix / 1e9, pix / gos, pix / pixels);
    if(nfit > 1) {
      const double b = (nfit * sxy - sx * sy) / (nfit * sxx - sx * sx), a0 = (sy - b * sx) / nfit;
      printf("      pixel-work fit: %.1f ns fixed per GO + %.2f ns per pixel; %.2f row segments per GO\n", a0 * ns, b * ns,
             (double)g_np_prof_rows / gos);
    }
    static const char *bn[4] = { "0 px", "1-32 px", "33-4095 px", ">=4096 px" };
    for(int i = 0; i < 4; i++) {
      if(bk_n[i] > 0) {
        printf("      pixel work, %-10s GOs %8.0f  %8.1f ns/GO  %6.2f ns/pixel\n", bn[i], bk_n[i], bk_t[i] * ns / bk_n[i],
               bk_p[i] > 0 ? bk_t[i] * ns / bk_p[i] : 0.0);
      }
    }
    printf("  (d) replay loop + timers     %8.3f s  %7.1f ns/GO\n", d / 1e9, d / gos);
    printf("  total                        %8.3f s  %7.1f ns/GO   (accesses/GO %.2f)\n", wall, wall * 1e9 / gos,
           (double)win.size() / gos);
  }
  else {
    printf("  loop %.1f ns/GO; do_command %.1f ns/GO of which pixel %.1f ns/GO\n", wall * 1e9 / gos, cmd / gos, pix / gos);
  }
  return (int)(sink & 0);
}
