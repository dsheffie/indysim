#ifndef __BBV_HH__
#define __BBV_HH__
/* SimPoint basic-block vectors (the interp_rv64 av class, --simpoint), for
 * data-driven phase strata: one vector per fixed instruction interval, keyed by
 * the PC a control transfer lands on, counting the instructions executed since
 * the previous transfer.  Dumped in the SimPoint .bb format ("T:id:count ..."),
 * ids assigned in order of first appearance. */
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

class bbv_log {
  uint64_t interval;
  uint64_t next_boundary;
  uint64_t bbsz = 0;
  std::vector<std::unordered_map<uint64_t, uint64_t>> vecs;
public:
  explicit bbv_log(uint64_t interval_) : interval(interval_), next_boundary(interval_) {
    vecs.emplace_back();
  }
  /* called once per retired instruction: pc_before/pc_after around execMips */
  inline void step(uint64_t pc_before, uint64_t pc_after, uint64_t icnt) {
    bbsz++;
    if(pc_after != pc_before + 4) {
      vecs.back()[pc_after] += bbsz;
      bbsz = 0;
    }
    if(icnt >= next_boundary) {
      next_boundary += interval;
      vecs.emplace_back();
    }
  }
  void dump(const std::string &fname) const {
    std::ofstream out(fname);
    std::unordered_map<uint64_t, uint64_t> id;
    uint64_t next_id = 1;
    for(const auto &v : vecs) {
      if(v.empty()) {
        continue;
      }
      out << "T";
      for(const auto &p : v) {
        auto it = id.find(p.first);
        if(it == id.end()) {
          it = id.emplace(p.first, next_id++).first;
        }
        out << ":" << it->second << ":" << p.second << " ";
      }
      out << "\n";
    }
  }
};
#endif
