/* np_min_stubs.cc -- device entry points sparse_mem.cc references, for building
 * np_replay without the rest of the ISS (e.g. natively on the FPGA board's ARM):
 * replay drives sgi_newport directly and never routes a CPU access. */
#include <cstdio>
#include <cstdlib>
#include "../sgi_mc.hh"
#include "../sgi_hpc.hh"
#include "../sgi_scc.hh"

uint32_t sgi_hpc::read(uint32_t, size_t) { abort(); }
void sgi_hpc::write(uint32_t, uint32_t, size_t) { abort(); }
uint32_t sgi_mc::read(uint32_t, size_t) { abort(); }
void sgi_mc::write(uint32_t, uint32_t, size_t) { abort(); }
uint8_t sgi_scc::read(uint32_t) { abort(); }
void sgi_scc::write(uint32_t, uint8_t) { abort(); }
