#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>

#ifdef __amd64__
#include <x86intrin.h>
#endif

#include "sparse_mem.hh"
#include "interpret.hh"
#include "sgi_mc.hh"
#include "sgi_hpc.hh"
#include "sgi_scc.hh"
#include "sgi_indy.hh"
#include "sgi_newport.hh"

/* IRIX serial console (IOC2 Z8530 SCC), 16-byte window inside HPC space. */
static const uint64_t SCC_BASE = 0x1fbd9830ULL;
static const uint64_t SCC_END  = 0x1fbd983fULL;


#define PROT (PROT_READ | PROT_WRITE)
#define MAP (MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE)

#ifdef ENABLE_O32_TRACE
/* WRTRACK: last-writer provenance, one entry per guest word (indexed by PA>>2).
 * mmap'd NORESERVE so only touched guest memory costs RAM (a std::map over an
 * 8.7B-insn boot would OOM). Follows a nullptr backwards. */
uint32_t *g_wr_pc   = nullptr;
uint64_t *g_wr_icnt = nullptr;
bool      g_wrtrack = false;
/* register-load provenance: for each GPR, the VA/PA/pc of the load that last wrote
 * it (WRTRACK). Lets a null pointer (base/ra/t9) be traced to its source word. */
uint32_t g_gpr_ld_va[32] = {0};
uint32_t g_gpr_ld_pa[32] = {0};
uint32_t g_gpr_ld_pc[32] = {0};
bool g_ramcap = false;
void ramcap_log(char rw, uint32_t addr) {
  static int n = 0;
  if(n++ < 60)
    fprintf(stderr, "[RAMCAP] %c PA=%08x pc=%08x icnt=%lu\n", rw, addr,
            (uint32_t)g_cur_pc, (unsigned long)g_cur_icnt);
}
#endif

sparse_mem::sparse_mem() {
  void* mempt = mmap(nullptr, sparse_mem::sz, PROT, MAP, -1, 0);
  mem = reinterpret_cast<uint8_t*>(mempt);
  assert(mem != reinterpret_cast<uint8_t*>(~0UL));
  assert(madvise(mem, 1UL<<32, MADV_DONTNEED)==0);
  //memset(mem, 0, sparse_mem::sz);
#ifdef ENABLE_O32_TRACE
  g_ramcap = getenv("RAMCAP") != nullptr;
  g_wrtrack = getenv("WRTRACK") != nullptr;
  if(g_wrtrack) {
    /* sz>>2 words; NORESERVE => backing follows the touched working set only. */
    g_wr_pc   = reinterpret_cast<uint32_t*>(mmap(nullptr, (sz>>2)*sizeof(uint32_t), PROT, MAP, -1, 0));
    g_wr_icnt = reinterpret_cast<uint64_t*>(mmap(nullptr, (sz>>2)*sizeof(uint64_t), PROT, MAP, -1, 0));
    assert(g_wr_pc   != reinterpret_cast<uint32_t*>(~0UL));
    assert(g_wr_icnt != reinterpret_cast<uint64_t*>(~0UL));
  }
#endif
}

sparse_mem::~sparse_mem() {
  munmap(mem, sz);
  mem = nullptr;
}

void sparse_mem::clear() {
  if(mem) {
    munmap(mem, sz);
  }
  void* mempt = mmap(nullptr, sparse_mem::sz, PROT, MAP, -1, 0);
  mem = reinterpret_cast<uint8_t*>(mempt);
  assert(mem != reinterpret_cast<uint8_t*>(~0UL));
  assert(madvise(mem, 1UL<<32, MADV_DONTNEED)==0);
}

#if 0
uint8_t *sparse_mem::add_page(uint64_t addr) {
  uint64_t paddr = addr / pgsize;
  uint8_t *pg = nullptr;
  //std::cerr << "ADDING PAGE " << std::hex << (paddr * pgsize) << std::dec << "\n";
  pg = reinterpret_cast<uint8_t*>(mmap(nullptr, pgsize, PROT, MAP, -1, 0));
  assert(pg != reinterpret_cast<uint8_t*>(~0UL));
  allocations[pg] = pgsize;
  memset(pg,0x0,pgsize);    
  smem[paddr] = pg;
  return pg;
}  

uint8_t* sparse_mem::get_page(uint64_t addr) {
  uint64_t paddr = addr / pgsize;
  //std::cerr << "trying to find page " << std::hex << addr << std::dec << "\n";
  auto it = smem.find(paddr);
  if(it == smem.end()) {
    return add_page(addr);
  }
  return it->second;
}

void sparse_mem::coalesce(uint64_t addr, uint64_t sz) {
  uint64_t pstart = addr / pgsize, eaddr = (addr + sz);
  uint64_t pstop = (eaddr + pgsize - 1) / pgsize;
  bool need_coalesce = false;
  //std::cout << "start = " << pstart*pgsize << "\n";
  //std::cout << "stop = " << pstop*pgsize << "\n";
  for(uint64_t paddr = pstart+1; paddr < pstop; ++paddr) {
    if(smem[paddr] != (smem[paddr-1] + pgsize)) {
      need_coalesce = true;
      std::cout << "need to coalesce\n";
      break;
    }
  }
  if(!need_coalesce) {
    return;
  }
}

void sparse_mem::prefault(uint64_t addr, uint64_t alloc_size) {
  uint64_t paddr = addr / pgsize, eaddr = (addr + alloc_size);
  bool any_present = false;
  
  uint64_t pstart = paddr, pstop = (eaddr + pgsize - 1) / pgsize;
  //std::cout << "pstart = " << pstart << ", pstop = " << pstop << "\n";

  for(uint64_t paddr = pstart; paddr < pstop; ++paddr) {
    if(smem.find(paddr) != smem.end()) {
      any_present = true;
      break;
    }
  }
  //any_present = true;
  /* create one big mmap */
  if(not(any_present)) {
    uint8_t *pg = nullptr;
    uint64_t allocsz = (pstop-pstart)*pgsize;
    //if(not(isPow2(allocsz))) {
    // allocsz = nextPow2(allocsz);
    //}
    //std::cout << "creating large allocation of "
    ///<< allocsz
    //	      << " bytes "
    //<< " from "
    //<< std::hex
    //<< pstart*pgsize
    //<< " to "
    //<< pstop*pgsize
    //<< "\n";
    
    pg = reinterpret_cast<uint8_t*>(mmap(nullptr, allocsz, PROT, MAP, -1, 0));
    assert(pg != reinterpret_cast<uint8_t*>(~0UL));
    allocations[pg] = allocsz;
    memset(pg,0x0,allocsz);
    for(uint64_t paddr = pstart, c = 0; paddr < pstop; ++paddr, ++c) {
      smem[paddr] = pg + (c*pgsize);
    }
  }
  else {
    /* add each page */
    for(uint64_t paddr = pstart; paddr < pstop; ++paddr) {
      if(smem.find(paddr) == smem.end()) {
	add_page(paddr * pgsize);
      }
    }
  }
}


uint32_t sparse_mem::crc32() const {
  uint32_t c = ~0x0;
#if 0
  //std::cout << present_bitvec.popcount()
  //<< " non-zero pages\n";
  for(size_t i = 0; i < npages; i++) {
#ifdef __amd64__
    if(present_bitvec[i]==false) {
      for(size_t n=0;n<4096;n++) {
	c = _mm_crc32_u8(c, 0);
      }
    }
    else {
      //uint8_t x = 0;
      for(size_t n=0;n<4096;n++) {
	c = _mm_crc32_u8(c, mem[i][n]);
	//x ^= mem[i][n];
      }
      //std::cout << "page " << i << " is non-zero, x = "
      //<< std::hex << static_cast<uint32_t>(x) << std::dec << "\n";
    }
#else
    static const uint32_t POLY = 0x82f63b78;
    for(size_t n=0;n<4096;n++) {
      uint8_t b = present_bitvec[i] ? mem[i][n] : 0;
      c ^= b;
      for(int k = 0; k < 8; k++) {
	c = c & 1 ? (c>>1) ^ POLY : c>>1;
      }
    }
#endif
  }
#endif
  return c ^ (~0x0);
}

void sparse_mem::clear() {
  for(auto o : allocations) {
    munmap(o.first, o.second);
  }
  allocations.clear();
  smem.clear();
}

sparse_mem::~sparse_mem() {
  for(auto o : allocations) {
    munmap(o.first, o.second);
  }
}

#endif




/* Which physical regions actually fall through to the flat backing store -- i.e.
 * what a strict device model (QEMU) would otherwise leave unmapped.  Real RAM
 * (low_local) and its low alias are skipped.  Enable with UNCLAIMEDDBG=1. */
static const bool unclaimed_log = getenv("UNCLAIMEDDBG") != nullptr;
namespace {
  struct unclaimed_tally {
    struct ent { uint64_t loads = 0, stores = 0, lo = ~0ULL, hi = 0; };
    std::map<mem_range_t, ent> m;
    void note(mem_range_t r, uint64_t pa, bool store) {
      if(r == mem_range_t::low_local || r == mem_range_t::sys_mem_alias) { return; }
      ent &e = m[r];
      if(store) { e.stores++; } else { e.loads++; }
      if(pa < e.lo) { e.lo = pa; }
      if(pa > e.hi) { e.hi = pa; }
    }
    ~unclaimed_tally() {
      if(not unclaimed_log) { return; }
      fprintf(stderr, "\n=== regions backed by the flat store (outside real RAM) ===\n");
      for(const auto &kv : m) {
        fprintf(stderr, "%-16s loads=%-12llu stores=%-12llu pa 0x%08llx..0x%08llx\n",
                rangeNames.at(kv.first).c_str(),
                (unsigned long long)kv.second.loads,
                (unsigned long long)kv.second.stores,
                (unsigned long long)kv.second.lo,
                (unsigned long long)kv.second.hi);
      }
    }
  };
  unclaimed_tally g_unclaimed;
}

template <typename T> T sparse_mem::route_load(uint64_t pa) {
  if(pa >= SCC_BASE && pa <= SCC_END) return static_cast<T>(st->scc->read((uint32_t)(pa - SCC_BASE)));
  if(st->np && pa >= sgi_newport::BASE && pa < sgi_newport::BASE + sgi_newport::SIZE) {
    return static_cast<T>(st->np->read((uint32_t)(pa - sgi_newport::BASE), sizeof(T)));
  }
  const mem_range_t r = compute_mem_range_type(static_cast<uint32_t>(pa));
  switch(r) {
  case mem_range_t::mc_regs:  return static_cast<T>(st->mc->read(pa & 0x1ffff, sizeof(T)));
  case mem_range_t::hpc_regs: return static_cast<T>(st->hpc->read(pa & 0x7ffff, sizeof(T)));
  default:
    if(unclaimed_log) { g_unclaimed.note(r, pa, false); }
    return *reinterpret_cast<T*>(mem+pa);
  }
}
template <typename T> void sparse_mem::route_store(uint64_t pa, T v) {
  if(pa >= SCC_BASE && pa <= SCC_END) { st->scc->write((uint32_t)(pa - SCC_BASE), (uint8_t)v); return; }
  if(st->np && pa >= sgi_newport::BASE && pa < sgi_newport::BASE + sgi_newport::SIZE) {
    uint64_t raw = 0;
    memcpy(&raw, &v, sizeof(T));               /* memory-order bytes, no sign extension */
    st->np->write((uint32_t)(pa - sgi_newport::BASE), raw, sizeof(T));
    return;
  }
  const mem_range_t r = compute_mem_range_type(static_cast<uint32_t>(pa));
  switch(r) {
  case mem_range_t::mc_regs:  st->mc->write(pa & 0x1ffff, static_cast<uint32_t>(v), sizeof(T)); break;
  case mem_range_t::hpc_regs: st->hpc->write(pa & 0x7ffff, static_cast<uint32_t>(v), sizeof(T)); break;
  default:
    if(unclaimed_log) { g_unclaimed.note(r, pa, true); }
    if((pa & ~0xfffULL) == 0x1fd00000ULL && v != 0) st->brk = 1;
    *reinterpret_cast<T*>(mem+pa) = v;
    break;
  }
}
#define INST(T) template T sparse_mem::route_load<T>(uint64_t); template void sparse_mem::route_store<T>(uint64_t, T);
INST(uint8_t) INST(uint16_t) INST(uint32_t) INST(uint64_t)
INST(int8_t) INST(int16_t) INST(int32_t) INST(int64_t)
#undef INST
