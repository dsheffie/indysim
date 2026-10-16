#include "sgi_mc.hh"
#include "helper.hh"
#include "interpret.hh"
#include "sgi_newport.hh"
#include <cstdio>
#include <cstdlib>

/* Define ENABLE_O32_TRACE to compile in the IRIX-memory debug tracing (MCFGLOG here,
 * plus WRTRACK/DEVMAP/PTETRACK/RAMCAP in interpret.cc/sparse_mem.*). Off by default. */
#ifdef ENABLE_O32_TRACE
static const bool g_mcfglog = getenv("MCFGLOG") != nullptr;   /* read-once debug knob */
#endif

/* device-trace spew off by default; set DEVTRACE=1 to re-enable. */
static const bool dev_verbose = getenv("DEVTRACE") != nullptr;
/* full MC register access log, for diffing against another implementation */
static const bool mc_access_log = getenv("MCDBG") != nullptr;
#define DPRINTF(...) do { if(dev_verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* https://erikarn.github.io/sgi/indy/datasheets/sgi_indy_mc.pdf */

/*
All of the MC registers will respond to two different addresses. It is up
to the programmer to use the correct address depending on the endian mode of
the processor.

The MC is connected to the least significant 32 bits of the sysad bus.
When a register is written the data must be driven on those bits.
When register is read the data will be returned on those pins as well.

If the processor is running in big endian mode the odd word addresses,
(addresses that end in 4 and 0xc) are used.

When the processor is running in little endian mode the even word addresses,
(addresses that end in 0 and 8) are used.
*/

/*
 * MC (Memory Controller) register map. `offs` is the byte offset into the MC
 * register window (physical 0x1fa00000..0x1fafffff, see sgi_indy.hh). Offsets
 * below are the big-endian "odd word" aliases (ending in 4/c) that IRIX and the
 * IP22 PROM use; each register also responds at the even-word alias (-4).
 *
 * Cross-checked against: SGI Indy MC datasheet (indy_docs/mc.pdf), Linux
 * arch/mips/include/asm/sgi/mc.h, and NetBSD arch/sgimips. Names follow mc.h.
 *
 *   0x004  cpuctrl0    CPU control word 0 (refresh, parity, watchdog, endian)
 *   0x00c  cpuctrl1    CPU control word 1 (GIO timeout, HPC/EXP endianness)
 *   0x014  watchdogt   Watchdog timer (read-only; any write clears it)
 *   0x01c  systemid    System ID: [3:0]=MC revision, [4]=EPRESENT (EISA present)
 *   0x02c  divider     RPSS divider (real-time counter prescale)
 *   0x034  eeprom      Serial EEPROM byte register (bit-banged NMC93xx)
 *   0x044  rcntpre     Refresh counter preload
 *   0x04c  rcounter    Refresh counter (read-only)
 *   0x084  giopar      GIO64 arbitration / bus parameter word
 *   0x08c  cputp       CPU bus arbitration time period
 *   0x09c  lbursttp    Long-burst time period
 *   0x0c4  mconfig0    Memory config bank 0/1 (bank valid, base addr, size)
 *   0x0cc  mconfig1    Memory config bank 2/3
 *   0x0d4  cmacc       CPU memory access config
 *   0x0dc  gmacc       GIO memory access config
 *   0x0e4  cerr        CPU error address (read-only)
 *   0x0ec  cstat       CPU error status (write clears)
 *   0x0f4  gerr        GIO error address (read-only)
 *   0x0fc  gstat       GIO error status (write clears)
 *   0x104  syssembit   System semaphore (single bit, test-and-set)
 *   0x10c  mlock       GIO memory access lock
 *   0x114  elock       EISA-from-GIO access lock
 *   0x154+ gio_dma_*   GIO DMA translation / control registers
 *   0x184+ dtlb_*      GIO DMA TLB (hi/lo pairs, 4 entries)
 *   0x1dc+ dma*        GIO DMA engine (size, stride, mode, count, start, run)
 *   0x1004 rpss        RPSS 32-bit free-running counter (read-only)
 */

uint32_t sgi_mc::read(uint32_t offs, size_t sz) {
  DPRINTF("read access to MC, reg %x pc %lx\n", offs, (unsigned long)s->pc);
  static const bool g_mclog = getenv("MCLOG") != nullptr;
  if(g_mclog and (uint32_t)s->pc >= 0x8800ab20u and (uint32_t)s->pc <= 0x8800b0b8u)
    fprintf(stderr, "[mc-r] pc=%08x reg=%03x\n", (uint32_t)s->pc, offs);
  uint32_t x = 0;
  if(dma_read(offs, &x)) {
    return x;
  }
  switch(offs)
    {
    case 0x0:    /* cpuctrl0 (even alias) */
    case 0x4:    /* cpuctrl0 CPU control word 0 */
    case 0x8:    /* cpuctrl1 (even alias) */
    case 0xc: {  /* cpuctrl1 CPU control word 1 */
      const uint32_t index = (offs >> 3) & 1;   /* cpuctrl0(0x0/0x4) vs cpuctrl1(0x8/0xc): bit 3, not bit 1 */
      x = cpu_control[index];
      break;
    }
    case 0xc4:   /* mconfig0 memory config banks 0/1 */
    case 0xcc: { /* mconfig1 memory config banks 2/3 */
      /* mconfig0=0xc4, mconfig1=0xcc differ in bit 3, NOT bit 1 -- (offs>>1)&1 gave
       * 0 for BOTH, so mconfig1 aliased mconfig0 and IRIX saw banks 2/3 == banks 0/1
       * => 512 MB instead of 256 MB, allocating frames into the 0x1f device window. */
      const uint32_t index = (offs >> 3) & 1;
      x = memcfg[index];
#ifdef ENABLE_O32_TRACE
      if(g_mcfglog) fprintf(stderr, "[MCFG] READ  mconfig%u offs=%02x sz=%zu -> %08x\n", index, offs, sz, x);
#endif
      break;
    }
    case 0x84:   /* giopar GIO64 arbitration / bus parameter word */
      x = gio64_arb_param;
      break;
    case 0xd4:   /* cmacc CPU memory access config */
      x = cpu_mem_access_config;
      break;
    case 0xdc:   /* gmacc GIO memory access config */
      x = gio_mem_access_config;
      break;
    case 0x1c:   /* systemid */
      /* MC System ID: low nibble = revision, bit4 (EPRESENT) = EISA present.
       * Indy has no EISA, so EPRESENT stays clear. */
      x = sys_id;
      break;
    case 0x30:   /* eeprom (even alias): serial EEPROM data bit, SDATAI high */
      x = 0x10;//eeprom_ctrl & (~0x10);
      break;
    case 0x1004: /* rpss free-running counter (read-only) */
      //printf("rpss counter read\n");
      x = static_cast<uint32_t>(s->icnt/10);//rpss_counter;
      break;
    default:
      DPRINTF("trying to read MC reg %x (lenient -> 0)\n", offs);
      x = 0;
      break;
    }
  //printf("read access to MC, reg %x, value %x\n", offs, x);  
  if(mc_access_log) {
    fprintf(stderr, "MC-R reg=%04x -> %08x sz=%zu\n", offs, x, sz);
  }
  return x;
}

uint32_t nbits = 0;
static uint8_t eerom[256] = {0};
static uint8_t byte = 0;
static uint32_t cbyte = 0;

void sgi_mc::write(uint32_t offs, uint32_t x, size_t sz) {
  if(mc_access_log) {
    fprintf(stderr, "MC-W reg=%04x <- %08x sz=%zu\n", offs, x, sz);
  }
  DPRINTF("write access to MC, reg %x, value %x, size %lu pc %lx\n", offs, x, sz, (unsigned long)s->pc);
  if(dma_write(offs, x)) {
    return;
  }
  static const bool g_mclogw = getenv("MCLOG") != nullptr;
  if(g_mclogw and (uint32_t)s->pc >= 0x8800ab20u and (uint32_t)s->pc <= 0x8800b0b8u)
    fprintf(stderr, "[mc-w] pc=%08x reg=%03x val=%02x\n", (uint32_t)s->pc, offs, x & 0xff);
  
  switch(offs)
    {
    case 0x0:    /* cpuctrl0 (even alias) */
    case 0x4:    /* cpuctrl0 CPU control word 0 */
    case 0xc: {  /* cpuctrl1 CPU control word 1 */
      const uint32_t index = (offs >> 3) & 1;   /* cpuctrl0(0x0/0x4) vs cpuctrl1(0x8/0xc): bit 3, not bit 1 */
      cpu_control[index] = x;
      break;
    }
    case 0x2c:   /* divider RPSS prescale */
      rpss_divider = x;
      break;
    case 0x84:   /* giopar GIO64 arbitration / bus parameter word */
      gio64_arb_param = x;
      break;
    case 0xc4:   /* mconfig0 memory config banks 0/1 */
    case 0xcc: { /* mconfig1 memory config banks 2/3 */
      const uint32_t index = (offs >> 3) & 1;   /* bit 3 selects mconfig0 vs mconfig1 (see read) */
      memcfg[index] = x;
#ifdef ENABLE_O32_TRACE
      if(g_mcfglog) fprintf(stderr, "[MCFG] WRITE mconfig%u offs=%02x sz=%zu <- %08x\n", index, offs, sz, x);
#endif
      break;
    }
    case 0xd4:   /* cmacc CPU memory access config */
      cpu_mem_access_config = x;
      break;
    case 0xdc:   /* gmacc GIO memory access config */
      gio_mem_access_config =x;
      break;
    case 0xec:   /* cstat CPU error status (any write clears) */
      cpu_error_status = 0;
      break;
    case 0xfc:   /* gstat GIO error status (any write clears) */
      gio_error_status = 0;
      break;
    case 0x30:   /* eeprom serial EEPROM control: bit-bang CS/CLK/DATA */
      eeprom_ctrl = x;
      if ( ((x>>1) & 3) == 3) {
	DPRINTF("data bit %d, bit %u\n", (x>>3)&1, nbits);
	byte = (byte << 1) | ((x>>3)&1);
	++nbits;	
	if(nbits==8) {
	  DPRINTF("wrote byte %u : %x\n", cbyte, (int)byte);	  
	  eerom[cbyte] = byte;
	  ++cbyte;
	  nbits = 0;
	  byte = 0;

	}

      }
      break;
    default:
      DPRINTF("write to unknown MC reg %x (lenient -> ignore)\n", offs);
      break;
    }
}

/* ---- GIO64 graphics DMA (MC spec "virtual DMA"; behaviour from MAME sgi_mc) ----
 * Each register answers at either word of its 8-byte slot (the spec's /4 and /c
 * aliases).  A start runs the whole transfer immediately; completion sets DMA_RUN
 * bit 3 and, when DMA_CTL bit 4 is set, DMA_CAUSE bit 3 + INT3 local0 MC_DMA. */
static const uint32_t DMA_MODE_TO_HOST = 1u << 1;
static const uint32_t DMA_MODE_FILL = 1u << 3;
static const uint32_t DMA_MODE_DIR = 1u << 4;

bool sgi_mc::dma_read(uint32_t offs, uint32_t *x) {
  const uint32_t r = offs & ~4u;
  if(r >= 0x180 && r <= 0x1b8) {
    const uint32_t i = (r - 0x180) >> 4;
    *x = ((r - 0x180) & 8) ? dma_tlb_lo[i] : dma_tlb_hi[i];
    return true;
  }
  switch(r)
    {
    case 0x150: *x = dma_gio_mask; return true;
    case 0x158: *x = dma_gio_sub; return true;
    case 0x160: *x = dma_cause; return true;
    case 0x168: *x = dma_ctl; return true;
    case 0x2000:
    case 0x2008: *x = dma_memadr; return true;
    case 0x2010: *x = dma_size; return true;
    case 0x2018: *x = dma_stride; return true;
    case 0x2020:
    case 0x2028: *x = dma_gio_adr; return true;
    case 0x2030: *x = dma_mode; return true;
    case 0x2038: *x = dma_count; return true;
    case 0x2040: *x = 0; return true;
    case 0x2048: *x = dma_run; return true;
    default:
      return false;
    }
}

bool sgi_mc::dma_write(uint32_t offs, uint32_t x) {
  const uint32_t r = offs & ~4u;
  if(r >= 0x180 && r <= 0x1b8) {
    const uint32_t i = (r - 0x180) >> 4;
    if((r - 0x180) & 8) {
      dma_tlb_lo[i] = x;
    }
    else {
      dma_tlb_hi[i] = x;
    }
    return true;
  }
  switch(r)
    {
    case 0x150: dma_gio_mask = x; return true;
    case 0x158: dma_gio_sub = x; return true;
    case 0x160:
      dma_cause = x;
      if(x == 0) {
        dma_irq = false;
      }
      return true;
    case 0x168: dma_ctl = x; return true;
    case 0x2000:
    case 0x2008: dma_memadr = x; return true;
    case 0x2010:
      dma_count = (dma_count & 0xffff0000u) | (x & 0xffffu);
      dma_size = x;
      return true;
    case 0x2018:
      dma_count = (dma_count & 0x0000ffffu) | (x & 0x03ff0000u);
      dma_stride = x;
      return true;
    case 0x2020: dma_gio_adr = x; return true;
    case 0x2028: dma_gio_adr = x; dma_perform(); return true;
    case 0x2030: dma_mode = x; return true;
    case 0x2038: dma_count = x; return true;
    case 0x2040:
      if(x & 1) {
        dma_perform();
      }
      return true;
    case 0x2070:                               /* GIO address + default params + start */
      dma_gio_adr = x;
      dma_size = 0x0001000c;
      dma_stride = 0x00010000;
      dma_count = 0x0001000c;
      dma_mode = 0x00000028;
      dma_perform();
      return true;
    default:
      return false;
    }
}

/* DMA TLB: 4 entries, each maps a 4 MB virtual region through a page of PTEs */
uint32_t sgi_mc::dma_translate(uint32_t va) {
  for(int e = 0; e < 4; e++) {
    if((va & 0xffc00000u) == (dma_tlb_hi[e] & 0xffc00000u)) {
      const uint32_t pte_pa = ((dma_tlb_lo[e] & 0x003fffc0u) << 6) + (((va & 0x003ff000u) >> 12) << 2);
      const uint32_t pte = __builtin_bswap32(s->mem.get<uint32_t>(pte_pa));
      return ((pte & 0x03ffffc0u) << 6) + (va & 0xfffu);
    }
  }
  /* MAME returns 0 here; use the address as physical instead of scribbling on PA 0 */
  static uint32_t nmiss = 0;
  if(nmiss++ < 8) {
    fprintf(stderr, "[mc-dma] TLB miss for %08x (treated as physical)\n", va);
  }
  return va;
}

static uint64_t dma_gio_read64(state_t *s, uint32_t gio) {
  if(s->np && gio >= sgi_newport::BASE && gio < sgi_newport::BASE + sgi_newport::SIZE) {
    return __builtin_bswap64(s->np->read(gio - sgi_newport::BASE, 8));
  }
  return __builtin_bswap64(s->mem.get<uint64_t>(gio));
}

static void dma_gio_write64(state_t *s, uint32_t gio, uint64_t v) {
  if(s->np && gio >= sgi_newport::BASE && gio < sgi_newport::BASE + sgi_newport::SIZE) {
    s->np->write(gio - sgi_newport::BASE, __builtin_bswap64(v), 8);
    return;
  }
  s->mem.set<uint64_t>(gio, __builtin_bswap64(v));
}

void sgi_mc::dma_perform() {
  static const bool g_dmalog = getenv("MCDMA_LOG") != nullptr;
  uint32_t mem = dma_memadr;
  uint32_t lines = dma_size >> 16;
  uint32_t zoom = (dma_count >> 16) & 0x3ff;
  uint32_t bytes = dma_count & 0xffff;
  const uint32_t width = dma_size & 0xffff, line_zoom = (dma_stride >> 16) & 0x3ff;
  const int16_t stride = (int16_t)dma_stride;
  const bool to_host = dma_mode & DMA_MODE_TO_HOST, fill = dma_mode & DMA_MODE_FILL;
  const bool up = dma_mode & DMA_MODE_DIR;
  if(g_dmalog) {
    fprintf(stderr, "[mc-dma] mem=%08x lines=%u width=%u zoom=%u stride=%d gio=%08x mode=%02x\n",
            mem, lines, width, line_zoom, (int)stride, dma_gio_adr, dma_mode);
  }
  uint32_t moved = 0;
  if(s->np) {
    s->np->set_in_dma(true);
  }
  dma_size &= 0x0000ffff;
  dma_count = 0;
  while(lines > 0) {
    lines--;
    while(zoom > 0) {
      zoom--;
      while(bytes > 0) {
        if(to_host && fill) {
          s->mem.set<uint32_t>(dma_translate(mem), __builtin_bswap32(dma_gio_adr));
          mem = up ? mem + 4 : mem - 4;
          bytes = (bytes >= 4) ? bytes - 4 : 0;
          moved += 4;
        }
        else if(to_host) {
          const uint32_t len = bytes < 8 ? bytes : 8;
          const uint64_t d = dma_gio_read64(s, dma_gio_adr);
          for(uint32_t i = 0; i < len; i++) {
            s->mem.set<uint8_t>(dma_translate(mem), (uint8_t)(d >> (56 - 8 * i)));
            mem = up ? mem + 1 : mem - 1;
          }
          bytes -= len;
          moved += len;
        }
        else {
          const uint32_t len = bytes < 8 ? bytes : 8;
          uint64_t d = 0;
          for(uint32_t i = 0; i < len; i++) {
            d |= (uint64_t)s->mem.get<uint8_t>(dma_translate(mem)) << (56 - 8 * i);
            mem = up ? mem + 1 : mem - 1;
          }
          dma_gio_write64(s, dma_gio_adr, d);
          bytes -= len;
          moved += len;
        }
      }
      bytes = width;
      if(zoom > 0) {
        mem = up ? mem - width : mem + width;
      }
    }
    zoom = line_zoom;
    mem += stride;
  }
  if(s->np) {
    s->np->set_in_dma(false);
  }
  sgi_newport::trace_dma(s->icnt, to_host, dma_gio_adr, moved);
  dma_memadr = mem;
  dma_run = (dma_run | (1u << 3)) & ~(1u << 6);
  if(dma_ctl & (1u << 4)) {
    dma_cause |= 1u << 3;
    dma_irq = true;
    s->irq_poke = true;
  }
}
