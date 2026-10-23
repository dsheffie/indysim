#ifndef __sparse_mem_hh__
#define __sparse_mem_hh__

#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cstdint>


#include <map>

#include "sim_bitvec.hh"
#include "helper.hh"
#include "cache_model.hh"   /* forward-decl-only wrt sparse_mem; safe to include here */
#include "ws_profile.hh"    /* data working-set profiler (g_wsprof) */

#ifndef unlikely
#define unlikely(x)    __builtin_expect(!!(x), 0)
#endif

#include <cstdio>

struct state_t;
class sgi_mc;
class sgi_hpc;

/* debug watchpoints, driven per-insn from main.cc's step loop.  Plain ints so the
 * header needs no state_t definition.  Used to trace which memory op writes a load's
 * address (store-watch on a PA) and to discover that PA (EA-print at a load PC). */
extern uint32_t g_watch_pa;      /* store-watch PA (0 = off) */
extern uint32_t g_watch_ldpc;    /* load-PC whose effective address to print (0 = off) */
extern uint64_t g_watch_lo, g_watch_hi;   /* icnt window [lo,hi) */
extern uint64_t g_cur_pc, g_cur_icnt;     /* current insn context */
extern uint64_t g_htrace;                 /* >0: trace next N executed pcs (kernel TLB-Mod handler) */

/* Define ENABLE_O32_TRACE to compile in the IRIX-memory debug instrumentation
 * (WRTRACK last-writer + register-load provenance, RAMCAP). Off by default. */
#ifdef ENABLE_O32_TRACE
/* WRTRACK: last-writer provenance -- per guest word (indexed by PA>>2), the pc+icnt
 * of the last store. mmap'd NORESERVE (see sparse_mem.cc). Follows a nullptr back to
 * whoever last wrote the source global. */
extern uint32_t *g_wr_pc;
extern uint64_t *g_wr_icnt;
extern bool      g_wrtrack;
extern uint32_t g_gpr_ld_va[32];   /* per-GPR: VA of the load that last wrote it */
extern uint32_t g_gpr_ld_pa[32];   /* per-GPR: PA of that load */
extern uint32_t g_gpr_ld_pc[32];   /* per-GPR: pc of that load (0 = none) */
extern bool g_ramcap;              /* RAMCAP: cap RAM at real 256MB (0x18000000-0x1effffff = not RAM) */
void ramcap_log(char rw, uint32_t addr);
#endif

class sparse_mem {
public:
  static const uint64_t pgsize = 4096;
  /* 1ULL, not 1UL: unsigned long is 32 bits on 32-bit targets (wasm32, i386),
   * where 1UL<<32 is undefined and rejects as a non-constant initializer. */
  static const uint64_t sz = 1ULL<<32;
  /* native: a single MAP_NORESERVE mmap over the whole 4 GiB space, pages committed
   * lazily by the OS.  wasm32 can neither reserve nor commit a flat 4 GiB map, so it
   * backs memory with an on-demand PAGE DIRECTORY instead (pages[] below): only pages
   * the guest actually touches cost memory.  This also lets the real SGI PROM's memory
   * sizing reach its test aperture at phys 0x20000000, which overflowed the old 512 MiB
   * flat wasm cap and made the PROM report "No usable memory found". */
  uint8_t *mem = nullptr;          /* native: 4 GiB mmap.  wasm: flat low window [0,flat_sz) */
#ifdef __EMSCRIPTEN__
  /* Hybrid backing: a flat calloc covers the low window [0, flat_sz) -- all RAM
   * (0x08000000+256M), the low MC alias, the device window and the PROM @0x1fc00000,
   * i.e. every hot access -- so execution and the PROM memory test run at flat speed.
   * Only addresses >= flat_sz (the real PROM's memory-sizing aperture at 0x20000000,
   * which the pure-512MiB-cap build dropped -> "No usable memory found") go through an
   * on-demand page directory, costing memory only for pages actually touched. */
  static const uint64_t flat_sz = 0x20000000ULL;   /* 512 MiB flat low window */
  static const uint64_t npages  = sz / pgsize;      /* page dir covers the full 4 GiB */
  uint8_t **pages = nullptr;
  inline uint8_t *page_byte(uint64_t a) {           /* only called for a >= flat_sz */
    uint8_t *&pg = pages[a / pgsize];
    if(pg == nullptr) { pg = reinterpret_cast<uint8_t*>(calloc(1, pgsize)); }
    return pg + (a % pgsize);
  }
#endif
  /* backing byte for an already-routed physical address (flat on native; flat low
   * window + paged high aperture on wasm) */
  inline uint8_t *backing(uint64_t a) {
#ifdef __EMSCRIPTEN__
    return (a < flat_sz) ? (mem + a) : page_byte(a);
#else
    return mem + a;
#endif
  }
  state_t *st = nullptr;
  bool route_devices = false;
  /* set per-access by va_translate: true only for a CACHED cpu load/store (kseg0
   * or a mapped C==3 page, and never a fetch).  DMA (get_raw_ptr) and MMIO leave
   * it as-is/false, so they always hit backing DRAM directly. */
  bool cache_active = false;
public:
  sparse_mem();
  ~sparse_mem();
  void clear();

  uint8_t * operator[](uint64_t addr) {
    return backing(addr);
  }
  uint8_t * operator+(uint64_t disp) {
    return (*this)[disp];
  }
#ifndef __EMSCRIPTEN__
  bool compare(const sparse_mem &other, bool verbose = false) {
    bool error = false;
    for(uint64_t b = 0; b < sz; ++b) {
      if(mem[b] != other.mem[b]) {
	error = true;
	if(verbose) {
	  std::cout << "byte " << std::hex << b
		    << " differs "
		    << static_cast<int>(mem[b])
		    << " vs "
		    << static_cast<int>(other.mem[b])
		    << std::dec
		    << "\n";
	}
      }
    }
    return error;
  }
#endif
  /* MC "System Memory Alias" (mc.pdf p.22): physical 0x0..0x7ffff is a hard
   * alias of DRAM 0x08000000..0x0807ffff -- main local memory lives at phys
   * 0x08000000, and the bottom 512 KB mirrors its base so the CPU exception
   * vectors (phys 0x0 / 0x80) and the SPB/romvec the FSBL stages at phys 0x1000
   * reach the same DRAM the kernel sees at 0x08000000. */
  static inline uint64_t mc_alias(uint64_t pa) {
    return (pa < 0x00080000UL) ? (pa + 0x08000000UL) : pa;
  }
  uint8_t *get_raw_ptr(uint64_t byte_addr) {
    byte_addr &= ((1ULL<<32) - 1);   /* 1ULL: 1UL<<32 is undefined on 32-bit targets */
    if(route_devices) byte_addr = mc_alias(byte_addr);
    return backing(byte_addr);
  }
  template <typename T>
  T get(uint64_t byte_addr) {
#ifdef ENABLE_O32_TRACE
    if(unlikely(g_ramcap) && (uint32_t)byte_addr >= 0x18000000u && (uint32_t)byte_addr < 0x1f000000u) {
      /* real DRAM is 256 MB (ends at 0x18000000); this gap (below the 0x1f device
       * window) is NOT RAM on henny. Return non-echoing so a sizing PROBE fails. */
      ramcap_log('r', (uint32_t)byte_addr);
      return (T)(~0ULL);
    }
#endif
    if(g_wsprof && cache_active) { g_wsprof->access((uint32_t)byte_addr); }
    if(g_cmodel && cache_active) {   /* cached cpu load -> the write-back model */
      T v; cm_load(g_cmodel, (uint32_t)byte_addr, &v, sizeof(T)); return v;
    }
    if(unlikely(g_watch_ldpc && (uint32_t)g_cur_pc == g_watch_ldpc &&
                g_cur_icnt >= g_watch_lo && g_cur_icnt < g_watch_hi)) {
      uint64_t _ea = byte_addr;
      if(route_devices && !(byte_addr>=0x1f000000 && byte_addr<=0x1fffffff)) _ea = mc_alias(byte_addr);
      fprintf(stderr, "[ld] icnt=%lu pc=%08x ea=%08x val=%016llx sz=%zu\n",
              (unsigned long)g_cur_icnt, (uint32_t)g_cur_pc, (uint32_t)byte_addr,
              (unsigned long long)(uint64_t)*reinterpret_cast<T*>(backing(_ea)), sizeof(T));
    }
    if(route_devices) {
      if(byte_addr>=0x1f000000 && byte_addr<=0x1fffffff) return route_load<T>(byte_addr);
      byte_addr = mc_alias(byte_addr);
    }
    return *reinterpret_cast<T*>(backing(byte_addr));
  }
  template<typename T>
  void set(uint64_t byte_addr, T v) {
    //static_assert(sizeof(T) != 8);
#ifdef ENABLE_O32_TRACE
    if(unlikely(g_ramcap) && (uint32_t)byte_addr >= 0x18000000u && (uint32_t)byte_addr < 0x1f000000u) {
      ramcap_log('w', (uint32_t)byte_addr);   /* not RAM on henny -> drop the write */
      return;
    }
    if(unlikely(g_wrtrack)) {        /* record last-writer BEFORE the cache/route split */
      for(uint32_t i = 0; i < sizeof(T); i += 4) {
        uint64_t w = (byte_addr + i) >> 2;
        g_wr_pc[w]   = (uint32_t)g_cur_pc;
        g_wr_icnt[w] = g_cur_icnt;
      }
    }
#endif
    if(g_wsprof && cache_active) { g_wsprof->access((uint32_t)byte_addr); }
    if(g_cmodel && cache_active) {   /* cached cpu store -> the write-back model */
      cm_store(g_cmodel, (uint32_t)byte_addr, &v, sizeof(T)); return;
    }
    if(unlikely(g_watch_pa && (uint32_t)byte_addr == g_watch_pa &&
                g_cur_icnt >= g_watch_lo && g_cur_icnt < g_watch_hi))
      fprintf(stderr, "[wr] icnt=%lu pc=%08x pa=%08x val=%016llx sz=%zu\n",
              (unsigned long)g_cur_icnt, (uint32_t)g_cur_pc, (uint32_t)byte_addr,
              (unsigned long long)(uint64_t)v, sizeof(T));
    if(route_devices) {
      if(byte_addr>=0x1f000000 && byte_addr<=0x1fffffff) { route_store<T>(byte_addr, v); return; }
      byte_addr = mc_alias(byte_addr);
    }
    *reinterpret_cast<T*>(backing(byte_addr)) = v;
  }
  template <typename T> T route_load(uint64_t pa);
  template <typename T> void route_store(uint64_t pa, T v);
  uint64_t bytes_allocated() const {
    return sz;
  }
};



#endif
