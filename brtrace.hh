#ifndef __BRTRACE_HH__
#define __BRTRACE_HH__
/* Correct-path control-transfer trace for r9999's oracle branch predictor
 * (ooo_core ORACLE_BP): one record per retired control transfer, in order,
 * classified EXACTLY as predecode.sv does (pd != 0), with the architectural
 * outcome known once the delay slot has run.  Timing-free, so it only affects
 * predictions on the RTL side, never correctness. */
#include <cstdint>
#include <cstdio>

struct brtrace_rec_t {
  uint64_t pc;
  uint64_t target;
  uint32_t taken;
  uint32_t pd;   /* predecode class (diagnostic) */
};

/* C++ copy of predecode.sv (insn already in host order) */
static inline uint32_t predecode_mips(uint32_t insn) {
  const uint32_t op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
  switch(op)
    {
    case 0:
      if((insn & 63) == 8) { return (rs == 31) ? 7 : 4; }
      if((insn & 63) == 9) { return 6; }
      return 0;
    case 1:
      switch(rt)
        {
        case 0: case 1: return 1;
        case 2: case 3: return 2;
        case 16: case 17: case 18: case 19: return 9;
        default: return 0;
        }
    case 2: return 3;
    case 3: return 5;
    case 4: return (rs == 0 && rt == 0) ? 8 : 1;
    case 5: case 6: case 7: return 1;
    case 17:
      if(rs == 8) { return ((insn >> 17) & 1) ? 2 : 1; }
      return 0;
    case 20: case 21: case 22: case 23: return 2;
    default: return 0;
    }
}

class brtrace_log {
  FILE *fp;
public:
  uint64_t n = 0;
  explicit brtrace_log(const char *path) : fp(fopen(path, "wb")) {}
  ~brtrace_log() { if(fp) { fclose(fp); } }
  bool ok() const { return fp != nullptr; }
  static inline bool is_vector(uint64_t pc) {
    const uint32_t v = (uint32_t)pc;
    return v == 0x80000000u || v == 0x80000080u || v == 0x80000180u ||
      v == 0xbfc00000u || v == 0xbfc00200u || v == 0xbfc00380u;
  }
  /* call after each execMips(): execMips runs a branch AND its delay slot in one
   * step (run_delay_slot), so pc_after is already the resolved successor; a
   * nullified likely also lands on pc+8 (not taken).  A step that lands on an
   * exception vector did not retire (a faulting delay slot re-runs its branch
   * from EPC), so it is not recorded. */
  inline void step(uint64_t pc_before, uint32_t insn, uint64_t pc_after) {
    if(predecode_mips(insn) == 0 || is_vector(pc_after)) {
      return;
    }
    brtrace_rec_t r{pc_before, pc_after, (pc_after != pc_before + 8) ? 1u : 0u, predecode_mips(insn)};
    fwrite(&r, sizeof(r), 1, fp);
    n++;
  }
};
#endif
