#ifndef __INTERRUPT_HH__
#define __INTERRUPT_HH__
/* The per-instruction part of the CP0 timer / interrupt model, inline so the main loop
 * pays no call: SCC transmit timing, the Newport retrace timer, Count/Compare (IP7),
 * then every INT_POLL instructions (or on irq_poke) the out-of-line device poll and Int
 * delivery.  See maybe_take_interrupt / device_poll_and_deliver in interpret.cc. */
#include "interpret.hh"
#include "sgi_scc.hh"

extern const uint64_t g_int_poll_mask;
void newport_retrace(state_t *s) __attribute__((noinline));
void device_poll_and_deliver(state_t *s);

static inline void maybe_take_interrupt_inline(state_t *s) {
  if(s->scc) {
    s->scc->tick1();                                 /* serial TX timing: keep exact */
  }
  if(__builtin_expect(s->icnt >= s->gfx_next_icnt, 0)) {
    newport_retrace(s);                              /* Newport vertical retrace timer */
  }
  /* CP0 Count is architecturally visible and the kernel's delay/clock calibration reads
   * it, so advance it (and latch the timer IP7) every instruction */
  s->cpr0[CPR0_COUNT] = (s->cpr0[CPR0_COUNT] + 1u) & 0xffffffffu;
  if(s->cpr0[CPR0_COUNT] == s->cpr0[CPR0_COMPARE]) {
    s->cpr0[CPR0_CAUSE] |= (1u << 15);               /* IP[7] = timer */
  }
  if(__builtin_expect(s->irq_poke || (s->icnt & g_int_poll_mask) == 0, 0)) {
    device_poll_and_deliver(s);
  }
}

#endif
