/* np_pack -- convert a NEWPORT_TRACE window into pre-decoded per-GO packets, the
 * format a PL shadow register file could hand the ARM.
 *
 *   np_pack TRACE --lo ICNT --hi ICNT OUT.pkt OUT.expect
 *
 * OUT.pkt (host byte order):
 *   header  u32 'NPPK', u32 rex words W, u32 GO count,
 *           u32 rex[W] + rb_snap_t at the window start,
 *           u32 rgbci[FB_W*FB_H], u32 cidaux[FB_W*FB_H]
 *   packets u8 opcode | adrmode << 2, u8 flags (bit0: rb_snap_t follows),
 *           u8 dirty bitmap[(W+7)/8] over the DECODED REX3 words since the previous
 *           GO finished, u32 value per dirty word, [rb_snap_t]
 * OUT.expect: per GO u64 FNV-1a of (rex words + rb_snap_t after the GO) and u64 pixels;
 *           then u64 FNV-1a of both framebuffers at the end.
 * Prints the packet size distribution. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
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

static uint64_t fnv64(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
  for(size_t i = 0; i < n; i++) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

static size_t W;
static std::vector<uint32_t> last;          /* decoded state when the previous GO finished */
static sgi_newport::rb_snap_t last_rb;
static FILE *pkt;
static std::map<size_t, uint64_t> size_hist;
static uint64_t pkt_bytes = 0, npkt = 0, dirty_words = 0;
static std::vector<uint64_t> word_dirty_count;

static void on_go(sgi_newport *np) {
  const uint32_t *w = np->rex_words();
  std::vector<uint8_t> rexb(W * 4);
  sgi_newport::rb_snap_t rb;
  np->get_state(rexb.data(), &rb);
  std::vector<uint8_t> bm((W + 7) / 8, 0);
  std::vector<uint32_t> vals;
  for(size_t i = 0; i < W; i++) {
    if(w[i] != last[i]) {
      bm[i >> 3] |= (uint8_t)(1u << (i & 7));
      vals.push_back(w[i]);
      word_dirty_count[i]++;
    }
  }
  const bool rbc = memcmp(&rb, &last_rb, sizeof(rb)) != 0;
  /* opcode/adrmode decoded from DRAWMODE0 (the first word of rex3_t) */
  const uint32_t dm0 = w[0];
  const uint8_t hdr[2] = { (uint8_t)((dm0 & 3) | (((dm0 >> 2) & 7) << 2)), (uint8_t)(rbc ? 1 : 0) };
  fwrite(hdr, 2, 1, pkt);
  fwrite(bm.data(), bm.size(), 1, pkt);
  fwrite(vals.data(), 4, vals.size(), pkt);
  if(rbc) {
    fwrite(&rb, sizeof(rb), 1, pkt);
  }
  const size_t sz = 2 + bm.size() + 4 * vals.size() + (rbc ? sizeof(rb) : 0);
  size_hist[sz]++;
  pkt_bytes += sz;
  dirty_words += vals.size();
  npkt++;
}

int main(int argc, char **argv) {
  if(argc < 8) {
    fprintf(stderr, "usage: %s TRACE --lo ICNT --hi ICNT OUT.pkt OUT.expect\n", argv[0]);
    return 1;
  }
  const uint64_t lo = strtoull(argv[3], nullptr, 0), hi = strtoull(argv[5], nullptr, 0);
  FILE *f = fopen(argv[1], "rb");
  if(f == nullptr) {
    perror(argv[1]);
    return 1;
  }
  std::vector<trace_rec_t> recs;
  trace_rec_t r;
  while(fread(&r, sizeof(r), 1, f) == 1 && r.icnt < hi) {
    recs.push_back(r);
  }
  fclose(f);
  sparse_mem *sm = new sparse_mem();
  state_t *s = new state_t(*sm);
  sgi_newport *np = new sgi_newport(s);
  s->np = np;
  W = sgi_newport::rex_size() / 4;
  last.resize(W);
  word_dirty_count.assign(W, 0);
  pkt = fopen(argv[6], "wb");
  FILE *ex = fopen(argv[7], "wb");
  bool started = false;
  uint64_t ngo = 0;
  long count_pos = 0;
  const size_t fbn = sgi_newport::FB_W * sgi_newport::FB_H;
  for(const trace_rec_t &t : recs) {
    if(t.type != 0) {
      continue;
    }
    if(!started && t.icnt >= lo) {
      /* window start: header with the full state and both framebuffers */
      started = true;
      const uint32_t magic = 0x4b50504e, w32 = (uint32_t)W, zero = 0;
      fwrite(&magic, 4, 1, pkt);
      fwrite(&w32, 4, 1, pkt);
      count_pos = ftell(pkt);
      fwrite(&zero, 4, 1, pkt);
      std::vector<uint8_t> rexb(W * 4);
      np->get_state(rexb.data(), &last_rb);
      memcpy(last.data(), rexb.data(), W * 4);
      fwrite(rexb.data(), 4, W, pkt);
      fwrite(&last_rb, sizeof(last_rb), 1, pkt);
      fwrite(np->fb_word(0), 4, fbn, pkt);
      fwrite(np->fb_word(0x80000000u), 4, fbn, pkt);
      sgi_newport::go_hook = on_go;
    }
    s->icnt = t.icnt;
    const uint64_t before = npkt;
    if(t.rw & 1) {
      np->write(t.offset, to_mem_order(t.value, t.size), t.size);
    }
    else {
      (void)np->read(t.offset, t.size);
    }
    if(npkt != before) {                       /* a GO ran: record its result, track state */
      std::vector<uint8_t> rexb(W * 4);
      np->get_state(rexb.data(), &last_rb);
      memcpy(last.data(), rexb.data(), W * 4);
      const uint64_t e[2] = { fnv64(&last_rb, sizeof(last_rb), fnv64(rexb.data(), rexb.size())), np->last_go_pixels() };
      fwrite(e, 8, 2, ex);
      ngo++;
    }
  }
  const uint64_t fbh = fnv64(np->fb_word(0x80000000u), fbn * 4, fnv64(np->fb_word(0), fbn * 4));
  fwrite(&fbh, 8, 1, ex);
  fclose(ex);
  fseek(pkt, count_pos, SEEK_SET);
  const uint32_t n32 = (uint32_t)ngo;
  fwrite(&n32, 4, 1, pkt);
  fclose(pkt);
  printf("%llu GOs, rex3 state %zu words (bitmap %zu B); packets %llu bytes, %.1f B/GO, %.2f dirty words/GO; final fb %016llx\n",
         (unsigned long long)ngo, W, (W + 7) / 8, (unsigned long long)pkt_bytes, (double)pkt_bytes / npkt,
         (double)dirty_words / npkt, (unsigned long long)fbh);
  printf("packet size distribution (bytes: GOs):");
  for(const auto &kv : size_hist) {
    printf(" %zu:%llu", kv.first, (unsigned long long)kv.second);
  }
  printf("\nwords dirtied most often (word index: GOs):");
  std::vector<std::pair<uint64_t, size_t>> wd;
  for(size_t i = 0; i < W; i++) {
    if(word_dirty_count[i]) {
      wd.push_back({word_dirty_count[i], i});
    }
  }
  std::sort(wd.rbegin(), wd.rend());
  for(size_t i = 0; i < wd.size() && i < 16; i++) {
    printf(" %zu:%llu", wd[i].second, (unsigned long long)wd[i].first);
  }
  printf("\n");
  return 0;
}
