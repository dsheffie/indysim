/* web_main.cc -- emscripten entry point for the browser/WASM build.
 *
 * Replaces main.o in the emcc build (see Makefile.emcc). The native main.cc is
 * left untouched: its optional features (boost program_options, gdb RSP, cosim
 * shared memory, checkpoint serialization, SDL display, POSIX signals) pull in
 * libraries with no emscripten port, so this slimmer entry sets up exactly the
 * browser configuration (IP22 devices + Newport + PS/2 + SCSI) and runs the sim.
 *
 * The run loop is driven by emscripten_set_main_loop (chunked, non-blocking) so
 * the browser/worker event loop runs between chunks -- that is what lets serial
 * output flow out and input flow in while the sim runs. Under node it falls back
 * to setTimeout scheduling and still boots to the console.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <emscripten.h>

#include "interpret.hh"
#include "loadelf.hh"
#include "sparse_mem.hh"
#include "pseudo_bios.hh"
#include "sgi_mc.hh"
#include "sgi_hpc.hh"
#include "sgi_scc.hh"
#include "sgi_newport.hh"
#include "sgi_ps2.hh"
#include "sgi_scsi.hh"
#include "sgi_seeq.hh"
#include "globals.hh"
#include "inst_record.hh"

/* debug watchpoint globals (declared extern in sparse_mem.hh) -- defined in
 * main.cc for the native build; web_main.cc is the entry TU for the web build. */
uint32_t g_watch_pa = 0, g_watch_ldpc = 0;
uint64_t g_watch_lo = 0, g_watch_hi = ~0ULL;
uint64_t g_cur_pc = 0, g_cur_icnt = 0;
uint64_t g_htrace = 0;

namespace globals {
  bool enClockFuncts = false;
  uint64_t icountMIPS = 0;
  uint64_t cycle = 0;
  bool trace_retirement = false;
  bool exec_dbg = false;   /* no debug knobs in the browser: run execMips' fast instance */
  bool trace_fp = false;
  bool report_syscalls = false;
  retire_trace *retire_log = nullptr;
  FILE *pctrace = nullptr;
  uint32_t pctrace_start = 0x88005960u;
  bool pctrace_on = false;
  bool asidpc_armed = false;
  bool cpu_r5000 = false;
};

/* read an entire host/MEMFS file into a malloc'd buffer */
static uint8_t *slurp(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if(!f) { return nullptr; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = reinterpret_cast<uint8_t*>(malloc(n));
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if(got != (size_t)n) { free(buf); return nullptr; }
  *len_out = (size_t)n;
  return buf;
}

/* sim state, file-scope so the main-loop tick can reach it */
static sparse_mem *g_sm = nullptr;
static state_t    *g_state = nullptr;

/* Newport scanout frame (0x00RRGGBB from render(), converted to canvas RGBA).
 * DISP_W*DISP_H*4 = 5 MiB static. */
static uint32_t g_frame[sgi_newport::DISP_W * sgi_newport::DISP_H];

/* C->JS: hand a finished RGBA frame to the page. Runs in the worker's JS context
 * (the module lives in the worker), so postMessage goes worker->main thread. The
 * heap slice is copied + transferred so the sim can reuse g_frame next frame. */
EM_JS(void, web_post_frame, (const uint8_t *ptr, int w, int h), {
  if (typeof postMessage !== 'function') { return; }   /* plain node (no worker): skip */
  var bytes = HEAPU8.slice(ptr, ptr + w * h * 4);
  postMessage({ kind: 'frame', w: w, h: h, buf: bytes.buffer }, [bytes.buffer]);
});

/* C->JS: retired-instruction count, posted once per tick; the page turns successive
 * samples into a live rate (mega-insns/sec).  double carries icnt exactly to 2^53. */
EM_JS(void, web_post_stat, (double icnt), {
  if (typeof postMessage === 'function') { postMessage({ kind: 'stat', icnt: icnt }); }
});

/* C->JS: the git hash this wasm was built from (githash.cc), posted once at startup so
 * the page can tell a stale cached module from the build it advertises. */
extern const char *githash;
EM_JS(void, web_post_build, (const char *hash), {
  if (typeof postMessage === 'function') { postMessage({ kind: 'build', hash: UTF8ToString(hash) }); }
});

/* render one Newport frame every N ticks (~10 fps at ~25 MIPS / 500k-insn ticks) */
static const int WEB_FRAME_EVERY = 5;

/* instructions per main-loop tick. ~25 MIPS wasm => ~500k is ~20 ms of sim, a
 * reasonable slice: the event loop runs between ticks to flush serial + drain
 * input, and in a worker (no rAF) emscripten schedules ticks back-to-back so we
 * still run near full speed. */
static const int WEB_CHUNK = 500000;

static void web_tick() {
  state_t *s = g_state;
  int budget = WEB_CHUNK;
  while(budget-- > 0 && s->brk == 0 && s->icnt < s->maxicnt) {
    g_cur_pc = (uint64_t)s->pc; g_cur_icnt = s->icnt;
    maybe_take_interrupt(s);   /* CP0 timer + SCC TX drain + int delivery (per step) */
    execMips(s);
  }
  if(s->np) {
    static int fcount = 0;
    if(++fcount >= WEB_FRAME_EVERY) {
      fcount = 0;
      s->np->render(g_frame);                 /* 0x00RRGGBB per pixel */
      const int npix = (int)(sgi_newport::DISP_W * sgi_newport::DISP_H);
      for(int i = 0; i < npix; i++) {         /* -> canvas RGBA (LE 0xFFBBGGRR) */
        uint32_t p = g_frame[i];
        g_frame[i] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
      }
      web_post_frame(reinterpret_cast<const uint8_t*>(g_frame),
                     (int)sgi_newport::DISP_W, (int)sgi_newport::DISP_H);
    }
  }
  web_post_stat((double)s->icnt);          /* live instruction-rate readout on the page */
  if(s->brk != 0 || s->icnt >= s->maxicnt) {
    fprintf(stderr, "[web_main] loop end: pc=%08x icnt=%lu brk=%d (maxicnt=%lu)\n",
            (uint32_t)s->pc, (unsigned long)s->icnt, s->brk, (unsigned long)s->maxicnt);
    emscripten_cancel_main_loop();
  }
}

/* Browser input -> PS/2 model, called from JS (worker) via ccall. Events that
 * arrive before --newport set up the PS/2 controller (or on a serial-only boot)
 * are simply dropped. key name is one of the sgi_ps2 scan-table names ("a",
 * "enter", "shift", ...); dx/dy use the host convention (+y down); buttons is the
 * 3-bit PS/2 mask (bit0 left, bit1 right, bit2 middle) matching MouseEvent.buttons. */
extern "C" {
EMSCRIPTEN_KEEPALIVE
void web_key_event(int down, const char *name) {
  if(g_state && g_state->ps2) {
    g_state->ps2->key(std::string(name), down != 0);
  }
}
EMSCRIPTEN_KEEPALIVE
void web_mouse_event(int dx, int dy, int buttons) {
  if(g_state && g_state->ps2) {
    g_state->ps2->mouse(dx, dy, (uint8_t)buttons);
  }
}
} /* extern "C" */

int main(int argc, char *argv[]) {
  std::string filename, start_pc, disk, prom, cpu = "r4400";
  uint64_t maxinsns = ~(0ULL);
  bool newport = false;
  web_post_build(githash);

  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *def) -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
    };
    if(a == "-f" || a == "--file") { filename = next(""); }
    else if(a == "--prom") { prom = next(""); }
    else if(a == "--start-pc") { start_pc = next(""); }
    else if(a == "--disk") { disk = next(""); }
    else if(a == "-m" || a == "--maxicnt") { maxinsns = strtoull(next("0").c_str(), nullptr, 0); }
    else if(a == "--cpu") { cpu = next("r4400"); }
    else if(a == "--newport") { newport = true; }
    else { std::cerr << "web_main: ignoring unknown arg '" << a << "'\n"; }
  }
  if(cpu == "r5000") { globals::cpu_r5000 = true; }
  else if(cpu != "r4400") { std::cerr << "--cpu must be r4400 or r5000\n"; return 1; }

  g_sm = new sparse_mem();
  g_state = new state_t(*g_sm);
  state_t *s = g_state;
  s->maxicnt = maxinsns;
  s->mc  = new sgi_mc(s);
  s->hpc = new sgi_hpc(s);
  s->scc = new sgi_scc(s);
  /* always present the WD33C93 (empty bus if no disk) so the sgiwd93 probe's reset
   * completes -- otherwise a diskless Linux boot hangs polling ASR_INT. */
  s->scsi = new sgi_scsi(s, disk, "", false);
  s->seeq = new sgi_seeq(s);
  if(newport) {
    s->np  = new sgi_newport(s);
    s->ps2 = new sgi_ps2(s);
  }
  g_sm->st = s;
  g_sm->route_devices = true;        /* IP22: low-memory alias + device window */
  sparse_mem_update_hooks();

  initState(s);
  if(!filename.empty()) { load_elf(filename.c_str(), s); }   /* sets s->pc = entry */
  if(start_pc.empty()) { install_pseudo_bios(s, g_sm); }

  /* PROM firmware (henry_arcs.bin): flat FSBL at the reset vector phys 0x1fc00000.
   * With a kernel loaded, patch the kentry slot @0xBFC00008 with the ELF entry
   * (big-endian) so the FSBL jumps to the right kernel -- mirrors main.cc. */
  if(!prom.empty()) {
    size_t n = 0;
    uint8_t *buf = slurp(prom.c_str(), &n);
    if(!buf) { std::cerr << "cannot open prom " << prom << "\n"; return 1; }
    /* write through get_raw_ptr (one byte at a time) so the paged wasm backing
     * faults in each page; this bypasses device routing like a raw mem+ store did. */
    for(size_t i = 0; i < n; i++) { *g_sm->get_raw_ptr(0x1fc00000u + i) = buf[i]; }
    std::cerr << "loaded PROM firmware (" << n << " bytes) at phys 0x1fc00000\n";
    free(buf);
    if(!filename.empty()) {
      uint32_t kentry = (uint32_t)s->pc;
      for(int i = 0; i < 4; i++) {
        *g_sm->get_raw_ptr(0x1fc00008u + i) = (kentry >> (24 - 8 * i)) & 0xff;
      }
      std::cerr << "patched kentry slot @0xbfc00008 = " << std::hex << kentry << std::dec << "\n";
    }
  }

  if(!start_pc.empty()) {
    s->pc = (int64_t)(int32_t)(uint32_t)strtoul(start_pc.c_str(), nullptr, 0);
    std::cerr << "start pc = " << std::hex << (uint32_t)s->pc << std::dec << "\n";
  }

  /* chunked, non-blocking: fps=0 => rAF in the browser / setTimeout elsewhere;
   * simulate_infinite_loop=1 => main() does not return, runtime stays alive. */
  emscripten_set_main_loop(web_tick, 0, 1);
  return 0;
}
