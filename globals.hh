#ifndef __GLOBALSH__
#define __GLOBALSH__

#include <cstdio>
#include <cstdint>

class retire_trace;

namespace globals {
  extern bool enClockFuncts;
  extern uint64_t icountMIPS;
  extern uint64_t cycle;
  extern bool trace_retirement;
  extern bool exec_dbg;   /* run execMips' DBG instance (debug knobs / retire trace / PCTRACE) */
  extern bool trace_fp;
  extern bool report_syscalls;
  /* PCTRACEOUT co-sim trace: when pctrace != null, each retired instruction
   * (delay slots included) appends its 32-bit virtual PC, once execution first
   * reaches pctrace_start. */
  extern FILE *pctrace;
  extern uint32_t pctrace_start;
  extern bool pctrace_on;
  extern bool asidpc_armed;
  /* When non-null and trace_retirement is set, each retired instruction is
   * appended as an inst_record{pc=physical, vpc=virtual, inst} for the
   * rv64analyzer trace consumer. */
  extern retire_trace *retire_log;
  /* --cpu r5000: present an R5000PC Indy (PRId/FIR imp 0x23, 32K+32K L1, no L2)
   * and enable the MIPS IV instructions.  Default (false) is the R4400 / MIPS III
   * machine r9999 implements: MIPS IV ops raise RI (or FP Unimplemented). */
  extern bool cpu_r5000;
  /* --cpu r10000: present an R10000 (IP28): PRId 0x0925, FIR 0x0900, the R10000 Config
   * layout (32K+32K L1, 1 MB secondary with 128-byte lines), and a 64-entry TLB.  It
   * also sets cpu_r5000, which the ISA gates read as "a MIPS IV part". */
  extern bool cpu_r10000;
  /* --ip28: Indigo2 R10000 (IP28) board differences from the Indy, enough to run the
   * IP28 PROM further: INT2 + its 8254 at 0x1fbd9000 (the Indy's ISDN window) instead
   * of the IOC2's INT3 at 0x1fbd9880, and an MC reporting revision D, which the IP28
   * PROM requires.  The CPU stays the R4400 model; graphics is absent unless --newport. */
  extern bool ip28;
};

#endif
