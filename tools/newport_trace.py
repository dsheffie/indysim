#!/usr/bin/env python3
"""newport_trace.py TRACE [--lo ICNT] [--hi ICNT] [--frame-pixels N]

Summarise an interp_mips NEWPORT_TRACE file (packed 24-byte records, see
sgi_newport.cc): REX3 access rate per million instructions (peak and sustained),
read/write split, register-group mix, GO count and opcode mix, pixels per GO,
pattern (ZPATTERN) use, MC graphics-DMA bytes, and a frame-rate estimate.

Guest time: the ISS retires one instruction per 100 MHz cycle, so 100M
instructions = 1 s.  A "frame" is a GO that produced >= --frame-pixels pixels
(a viewport clear); that is a heuristic, stated in the output."""
import argparse
import collections
import numpy as np

REC = np.dtype([("type", "u1"), ("size", "u1"), ("rw", "u1"), ("aux", "u1"),
                ("offset", "<u4"), ("icnt", "<u8"), ("value", "<u8")])
OPS = ["noop", "read", "draw", "scr2scr"]
ADR = ["span", "block", "iline", "fline", "aline", "adr5", "adr6", "adr7"]


def group(off):
    o = off & ~0x800                            # the GO alias is the same register
    if o < 0x0040:
        return "mode/setup"                     # DRAWMODE, LS, pattern, colorback/vram, smask0, setup
    if 0x0100 <= o < 0x0160:
        return "iterators"                      # x/y start/end, bresenham, GL coords
    if 0x0200 <= o < 0x0230:
        return "color"                          # full-state colours, slopes, wrmask, colori
    if 0x0230 <= o < 0x0238:
        return "HOSTRW"
    if 0x0238 <= o < 0x0248:
        return "DCB"
    if 0x1300 <= o < 0x1348:
        return "config/status"                  # smask1-4, window, clipmode, config, status
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--lo", type=int, default=0)
    ap.add_argument("--hi", type=int, default=1 << 63)
    ap.add_argument("--frame-pixels", type=int, default=20000)
    ap.add_argument("--window", type=int, default=1000000)
    a = ap.parse_args()

    r = np.fromfile(a.trace, dtype=REC)
    r = r[(r["icnt"] >= a.lo) & (r["icnt"] < a.hi)]
    acc = r[r["type"] == 0]
    cpu = acc[(acc["rw"] & 2) == 0]
    go = r[r["type"] == 1]
    dma = r[r["type"] == 2]
    if len(cpu) == 0:
        print("no CPU accesses in range")
        return
    lo, hi = int(cpu["icnt"].min()), int(cpu["icnt"].max())
    span = max(hi - lo, 1)
    print("range: icnt %d .. %d (%.2f M insns, %.3f s guest @100MHz)" % (lo, hi, span / 1e6, span / 1e8))

    # rate in 1M-instruction windows
    w = (cpu["icnt"] - lo) // a.window
    counts = np.bincount(w.astype(np.int64))
    active = counts[counts > 0]
    print("\nCPU accesses: %d   (+%d issued by MC graphics DMA)" % (len(cpu), len(acc) - len(cpu)))
    print("  per %dM insns: peak %d, mean over active windows %.0f, mean over whole span %.0f"
          % (a.window // 1000000, counts.max(), active.mean(), len(cpu) / (span / a.window)))
    print("  active windows %d of %d" % (len(active), len(counts)))
    st = ((cpu["offset"] & ~0x800) >> 3) == (0x1338 >> 3)
    nst = cpu[~st]
    if len(nst):
        w2 = (nst["icnt"] - lo) // a.window
        c2 = np.bincount(w2.astype(np.int64))
        a2 = c2[c2 > 0]
        print("  excluding STATUS polls/acks (%d, incl. one retrace ack per vblank): peak %d, mean over active windows %.0f, active windows %d"
              % (int(st.sum()), c2.max(), a2.mean(), len(a2)))
        w10 = (nst["icnt"] - lo) // (10 * a.window)
        print("  sustained as NEWPORT_FEASIBILITY.md defines it (busiest 10M window / 10): %.0f"
              % (np.bincount(w10.astype(np.int64)).max() / 10.0))
    nw = int(((cpu["rw"] & 1) == 1).sum())
    print("  writes %d (%.2f%%), reads %d" % (nw, 100.0 * nw / len(cpu), len(cpu) - nw))
    sz = collections.Counter(cpu["size"].tolist())
    print("  access sizes:", ", ".join("%dB %.1f%%" % (k, 100.0 * v / len(cpu)) for k, v in sorted(sz.items())))

    g = collections.Counter(group(o) for o in cpu["offset"].tolist())
    print("\nregister-group mix:")
    for k, v in g.most_common():
        print("  %-14s %8d  %5.1f%%" % (k, v, 100.0 * v / len(cpu)))
    goalias = int(((cpu["offset"] & 0x800) != 0).sum())
    print("  (via the GO alias: %d)" % goalias)

    print("\nGO commands: %d   (%.1f per M insns over the span)" % (len(go), len(go) / (span / 1e6)))
    kinds = collections.Counter((int(x) & 3, (int(x) >> 2) & 7) for x in go["aux"])
    for (op, ad), v in kinds.most_common():
        sel = go[(go["aux"] & 3 == op) & ((go["aux"] >> 2) & 7 == ad)]
        print("  %-7s %-5s %8d  pixels %10d  (mean %.1f/GO)" % (OPS[op], ADR[ad], v, int(sel["value"].sum()),
                                                             sel["value"].mean() if len(sel) else 0))
    px = go["value"].astype(np.int64)
    edges = [0, 1, 2, 4, 8, 16, 32, 33, 64, 128, 256, 1024, 4096, 1 << 62]
    hist, _ = np.histogram(px, bins=edges)
    print("\npixels per GO:")
    for i in range(len(hist)):
        if hist[i]:
            print("  [%d, %d)  %d" % (edges[i], edges[i + 1] if i + 1 < len(edges) - 1 else -1, hist[i]))
    print("  total pixels %d, mean %.1f/GO" % (int(px.sum()), px.mean() if len(px) else 0))

    m0 = go["offset"]
    pat = (m0 >> 12) & 1
    len32 = (m0 >> 15) & 1
    print("\nZPATTERN (DRAWMODE0 bit12) GOs: %d (%.1f%%); with LENGTH32 (bit15): %d; patterned+LENGTH32 segments of <=32 px: %d"
          % (int(pat.sum()), 100.0 * pat.sum() / max(len(go), 1), int((pat & len32).sum()),
             int((((pat & len32) == 1) & (px <= 32)).sum())))
    zwr = int(((cpu["offset"] & ~0x800) == 0x14).sum())
    print("  ZPATTERN register writes (0x0014): %d" % zwr)

    print("\nMC graphics DMA: %d transfers, %d bytes (to host %d, from host %d)"
          % (len(dma), int(dma["value"].sum()), int(dma[dma["rw"] == 1]["value"].sum()),
             int(dma[dma["rw"] == 0]["value"].sum())))

    big = go[go["value"] >= a.frame_pixels]
    if len(big) >= 2:
        t = big["icnt"].astype(np.int64)
        fs = (len(big) - 1) / ((t[-1] - t[0]) / 1e8)
        print("\nframes (GOs with >= %d pixels): %d, %.2f per guest second between the first and last"
              % (a.frame_pixels, len(big), fs))
    else:
        print("\nframes: no GO with >= %d pixels -- no frame boundary identified" % a.frame_pixels)


if __name__ == "__main__":
    main()
