#!/usr/bin/env python3
"""Make the packaged ReShade effects compile with Wine's own d3dcompiler_47.

Under Proton the HLSL that ReShade generates is compiled by Wine's
d3dcompiler_47, which is vkd3d-shader, not Microsoft's compiler. Three of its
limits hit DLSS5_Feed.fx and vort_Motion.fx, measured with GE-Proton 11-6's
own DLL under Wine:

  * ReShade turns every `[loop]` into `[fastopt]` for shader model 4+, and
    vkd3d-shader rejects that attribute outright
    ("E5017: Aborting due to not yet implemented feature: Unhandled attribute
    'fastopt'"). It also ignores `[loop]` itself, so the attribute is simply
    dropped: every `[loop]` in both effects compiles without it.
  * vkd3d-shader unrolls any loop whose trip count it can evaluate, and gives
    up past 1024 iterations. The one such loop that breaks is
    DLSS5_Feed.fx's fit solve, 920 samples with a 9-wide `[unroll]` inside.
    Its bound is made opaque by adding a uniform that is always zero.
  * The same fit solve, compiled at all, takes vkd3d-shader 1.8 GB of a 32-bit
    process's 4 GB and is never returned, so a 32-bit game could not build the
    network on its first launch. Its only user, the experimental geometry
    vectors (off by default), becomes a preprocessor switch of the same name.

Both effects are MIT; the copies in the package carry a note of the change.
Every edit is anchored and asserted, so a new upstream revision that moves
the text fails the build rather than shipping unpatched.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
note = "// Modified for DLSS 5 NR on AMD: `[loop]` attributes removed so that Wine's\n" \
       "// d3dcompiler_47 (vkd3d-shader) accepts the HLSL ReShade generates; see\n" \
       "// linux/build/patch_reshade_effects.py in that project.\n"

def strip_loop(path: pathlib.Path) -> int:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    # The attribute may be followed by a line comment before the loop keyword.
    new, n = re.subn(r"\[loop\]\s*(?://[^\n]*)?\s*(?=(for|while|do)\b)", "", text)
    # VORT writes `[loop] // comment` above an `#endif` that precedes the loop.
    new, m = re.subn(r"\[loop\](\s*//[^\n]*\n\s*#endif)", r"\1", new)
    n += m
    if n:
        path.write_text(note + new, encoding="utf-8", errors="surrogateescape")
    return n

total = 0
feed = root / "DLSS5_Feed.fx"
text = feed.read_text(encoding="utf-8")
# The fit solve's two loop bounds become constant-buffer values with the same
# numbers as defaults. vkd3d-shader folds `x & 0` and `x * 0` away, so an
# arithmetic disguise does not work; a uniform it cannot see the value of does.
assert text.count("#define DLSS5_FIT_W 40\n") == 1 and text.count("#define DLSS5_FIT_H 23\n") == 1, \
    "DLSS5_Feed.fx: the fit grid defines moved"
old = "#define DLSS5_FIT_H 23\n"
new = ("#define DLSS5_FIT_H 23\n"
       "// Loop bounds the compiler cannot evaluate: vkd3d-shader (Wine's d3dcompiler_47)\n"
       "// unrolls every loop whose count it can see and fails past 1024 iterations, and\n"
       "// the fit solve is 2 x 920 with a 9-wide unroll inside. Same values, from a\n"
       "// constant buffer.\n"
       "uniform int DLSS5_FitTotal < hidden = true; > = 920;\n"
       "uniform int DLSS5_FitIterations < hidden = true; > = 2;\n")
text = text.replace(old, new, 1)
old = "    const int total = DLSS5_FIT_W * DLSS5_FIT_H;\n"
assert text.count(old) == 1, "DLSS5_Feed.fx: the fit-solve bound moved"
text = text.replace(old, "    const int total = DLSS5_FitTotal;\n")
old = "    [loop] for (int it = 0; it < 2; ++it)\n"
assert text.count(old) == 1, "DLSS5_Feed.fx: the fit-solve iteration loop moved"
text = text.replace(old, "    for (int it = 0; it < DLSS5_FitIterations; ++it)\n")
# An `[unroll]` loop nested inside another `[unroll]` loop makes vkd3d-shader
# give up ("Unable to unroll loop, maximum iterations reached (1024)"), bisected
# on the generated HLSL: with the two inner attributes gone the entry compiles.
# Nine iterations in a 1x1-pixel solve; a real loop costs nothing here.
for old in ("                [unroll] for (int j = i; j < 9; ++j) { M[k] += B[i] * B[j]; ++k; }\n",
            "                [unroll] for (int j = i; j < 9; ++j) { G[i * 11 + j] = M[k2]; G[j * 11 + i] = M[k2]; ++k2; }\n"):
    assert text.count(old) == 1, "DLSS5_Feed.fx: a nested unroll in the fit solve moved: " + old.strip()
    text = text.replace(old, old.replace("[unroll] ", ""))
# The experimental geometry vectors become a preprocessor switch (same name,
# default off) instead of a checkbox. As a uniform the compiler has to build
# PS_FitSolve even though it never runs; vkd3d-shader takes 1.8 GB of a 32-bit
# game's address space for it and never gives it back (4 GB total, measured
# with GE-Proton 11-7's d3dcompiler_47: 30 MB free afterwards), so the network
# could not be built on a first launch; a 64-bit process takes 2.6 GB and minutes.
# With the switch off the solve is not compiled at all.
old = """uniform bool GEOM_ENABLE <
    ui_category = "Geometry vectors (camera model + depth) -- EXPERIMENTAL";
    ui_label = "Use geometry vectors (experimental, off by default)";
"""
assert text.count(old) == 1, "DLSS5_Feed.fx: the GEOM_ENABLE uniform moved"
start = text.index(old)
end = text.index("> = false;\n", start) + len("> = false;\n")
text = text[:start] + (
    "// Modified for DLSS 5 NR on AMD: a preprocessor switch instead of a checkbox (set\n"
    "// GEOM_ENABLE=1 under the effect's preprocessor definitions to try it). Wine's\n"
    "// d3dcompiler_47 needs 1.8 GB and more for PS_FitSolve; off, it is not compiled.\n"
    "#ifndef GEOM_ENABLE\n"
    "    #define GEOM_ENABLE 0\n"
    "#endif\n") + text[end:]
old = "    if (!GEOM_ENABLE) { P0 = 0.0; P1 = 0.0; P2 = 0.0; P3 = 0.0; P4 = 0.0; P5 = 0.0; return; }\n"
assert text.count(old) == 1, "DLSS5_Feed.fx: the fit solve's early out moved"
text = text.replace(old, old + "#if GEOM_ENABLE\n")
old = "    P5 = float4(inlier, rms, used, 0.0);\n}\n"
assert text.count(old) == 1, "DLSS5_Feed.fx: the fit solve's end moved"
text = text.replace(old, "    P5 = float4(inlier, rms, used, 0.0);\n#endif\n}\n")
feed.write_text(text, encoding="utf-8")
n = strip_loop(feed)
assert n >= 4, f"DLSS5_Feed.fx: expected at least 4 [loop] attributes, found {n}"
total += n

vort = [root / "vort_Motion.fx", root / "vort_Static.fx"] + sorted((root / "Includes").glob("*.fxh"))
vn = sum(strip_loop(p) for p in vort)
assert vn >= 20, f"VORT: expected at least 20 [loop] attributes, found {vn}"
total += vn
print(f"patched {total} [loop] attributes, the fit-solve bound and the GEOM_ENABLE switch in {root}")
