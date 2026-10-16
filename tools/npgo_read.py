#!/usr/bin/env python3
"""npgo_read.py CAPTURE.zst [--summary] [--list N] [--op draw|read|scr2scr] [--adr span|block|...]
                           [--asid A] [--pc PC] [--icnt LO:HI]

Read an interp_mips NEWPORT_DRAWCAP stream (zstd, record format v1 documented in
sgi_newport.cc): print matching GO records, and/or summarise them by
opcode / address mode / ASID (process) with pixel and touched-word totals.
Checksum records are listed with --list and counted in the summary."""
import argparse
import collections
import struct
import subprocess

OPS = ["noop", "read", "draw", "scr2scr"]
ADR = ["span", "block", "iline", "fline", "aline", "adr5", "adr6", "adr7"]
RB_SNAP = struct.Struct("<IIBBBBBBBB")


def records(path):
    p = subprocess.Popen(["zstd", "-dc", path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    f = p.stdout

    def rd(n):
        b = f.read(n)
        if len(b) != n:
            raise EOFError
        return b
    try:
        while True:
            magic, ver, kind = struct.unpack("<IHH", rd(8))
            if magic != 0x4F47504E or ver != 1:
                raise SystemExit("bad record header %08x v%d" % (magic, ver))
            if kind == 2:
                icnt, ngo, h0, h1 = struct.unpack("<QQQQ", rd(32))
                yield {"kind": "sum", "icnt": icnt, "gos": ngo, "rgbci": h0, "cidaux": h1}
                continue
            icnt, pc = struct.unpack("<QI", rd(12))
            asid, op, adr, _ = struct.unpack("<BBBB", rd(4))
            pixels, rs = struct.unpack("<QI", rd(12))
            rd(rs)
            rb0 = RB_SNAP.unpack(rd(RB_SNAP.size))
            n = struct.unpack("<I", rd(4))[0]
            touched = rd(12 * n)
            rd(rs)
            rd(RB_SNAP.size)
            yield {"kind": "go", "icnt": icnt, "pc": pc, "asid": asid, "op": op, "adr": adr,
                   "pixels": pixels, "touched": n, "logicop": rb0[6], "planes": rb0[4],
                   "depth": rb0[5], "writes": sum(1 for i in range(n)
                                                  if struct.unpack_from("<III", touched, 12 * i)[1] !=
                                                  struct.unpack_from("<III", touched, 12 * i)[2])}
    except EOFError:
        pass
    p.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--list", type=int, default=0)
    ap.add_argument("--op")
    ap.add_argument("--adr")
    ap.add_argument("--asid", type=int)
    ap.add_argument("--pc", type=lambda s: int(s, 0))
    ap.add_argument("--icnt")
    a = ap.parse_args()
    lo, hi = 0, 1 << 64
    if a.icnt:
        l, h = a.icnt.split(":")
        lo, hi = int(l or 0), int(h or (1 << 64))
    by = collections.defaultdict(lambda: [0, 0, 0, 0])
    by_asid = collections.Counter()
    nsum = listed = 0
    for r in records(a.capture):
        if r["kind"] == "sum":
            nsum += 1
            if a.list and listed < a.list:
                print("checksum @icnt %d after %d GOs: rgbci %016x cidaux %016x" % (r["icnt"], r["gos"], r["rgbci"], r["cidaux"]))
                listed += 1
            continue
        if not (lo <= r["icnt"] < hi):
            continue
        if a.op and OPS[r["op"] & 3] != a.op:
            continue
        if a.adr and ADR[r["adr"] & 7] != a.adr:
            continue
        if a.asid is not None and r["asid"] != a.asid:
            continue
        if a.pc is not None and r["pc"] != a.pc:
            continue
        k = (OPS[r["op"] & 3], ADR[r["adr"] & 7])
        b = by[k]
        b[0] += 1
        b[1] += r["pixels"]
        b[2] += r["touched"]
        b[3] += r["writes"]
        by_asid[r["asid"]] += 1
        if a.list and listed < a.list:
            print("GO icnt %d pc %08x asid %d %s %s pixels %d touched %d changed %d planes %d depth %d logicop %d"
                  % (r["icnt"], r["pc"], r["asid"], k[0], k[1], r["pixels"], r["touched"], r["writes"],
                     r["planes"], r["depth"], r["logicop"]))
            listed += 1
    if a.summary or not a.list:
        tot = sum(v[0] for v in by.values())
        print("%d GO records, %d checksum records" % (tot, nsum))
        for k, v in sorted(by.items(), key=lambda kv: -kv[1][0]):
            print("  %-7s %-5s %9d GOs  pixels %11d  touched %11d  changed %11d" % (k[0], k[1], v[0], v[1], v[2], v[3]))
        print("  by ASID (process):", ", ".join("%d: %d" % kv for kv in by_asid.most_common(10)))


if __name__ == "__main__":
    main()
