#!/usr/bin/env python3
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
"""Compare the per-layer hashes of an nr_graph run with the RDNA3 golden, and name the first layer that differs.

    compare_layers.py layers.txt [golden]

layers.txt is the standard output of `nr_graph ... --source-width 1920 --source-height 1080 --value-stats` (the
1080p network of docs/ngx-verification). The golden is linux/test/golden/rdna3/layers-1080.txt. Layers are
bit-exact from run to run and from driver to driver when the shaders are compiled right, so the first row that
differs is the first kernel the driver got wrong; the rows are in execution order.
"""
import re
import sys
from pathlib import Path

GOLDEN = Path(__file__).resolve().parent / "golden" / "rdna3" / "layers-1080.txt"


def rows(text):
    """(value, view, layer, hash) from nr_graph's --value-stats rows, or from the golden's condensed rows."""
    out = []
    for line in text.splitlines():
        t = line.split()
        if not t or not t[0].isdigit() or not re.fullmatch(r"[0-9a-f]{1,16}", t[-1]):
            continue
        if len(t) == 4 and t[1] in ("twin", "arena"):
            out.append((int(t[0]), t[1], t[2], t[3]))
        elif len(t) > 8:
            out.append((int(t[0]), "twin" if "twin" in line else "arena", t[2], t[-1]))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    got = rows(Path(sys.argv[1]).read_text(errors="replace"))
    want = rows(Path(sys.argv[2] if len(sys.argv) > 2 else GOLDEN).read_text())
    if not got:
        sys.exit("no layer rows in " + sys.argv[1] + " (did nr_graph run to the end?)")
    gold = {(i, v): (n, h) for i, v, n, h in want}
    bad = [(i, v, n, h, gold.get((i, v))) for i, v, n, h in got if gold.get((i, v), (None, None))[1] != h]
    print(f"{len(got)} layers in the run, {len(want)} in the golden, {len(got) - len(bad)} identical, {len(bad)} different")
    for i, v, n, h, g in bad[:25]:
        print(f"  value {i:>4} {v:<5} {n:<8} got {h}  golden {g[1] if g else '(none)'}")
    if len(bad) > 25:
        print(f"  ... and {len(bad) - 25} more")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
