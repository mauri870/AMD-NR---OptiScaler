#!/usr/bin/env python3
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
"""Build the RDNA3 network for the AMD Windows driver (LLPC) from windows/shaders/rdna3/pipelines.json.

    build_network.py [rdna3] [--out DIR] [--check DIR]

Writes (default build/windows/rdna3/network):
    g_<name>.spv + the four marker files   the network, what --spv-dir points at
    runtime/                                 the passes around it (windows/shaders/rdna3/passes)
    temporal/                                the temporal variants and the motion estimator

The driver's compiler does not compile the Linux network as written: it crashes (an access violation inside
amdvlk64.dll while creating the pipeline) on every pipeline of fswin_t.comp, attn.comp and ffwd3_t.comp and on the
temporal variants of fswin_t, and it compiles them once their loops are unrolled. Those sources are preprocessed
(glslang -E), fully unrolled (windows/build/unroll_glsl.py) and compiled from the result; everything else is
compiled as it is. On RADV the unrolled set is bit-exact with the Linux one. The persistent kernels (fswinp*)
still crash the compiler even unrolled and are not used on this driver (NR_NO_PERSIST).

The host must be compiled with the same constants: windows/build/arch/rdna3.sh.
--check DIR compares every network SPV and marker with DIR byte for byte and
lists the differences (exit 1 if any).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
RUNTIME = ['runtime_alpha', 'runtime_encode', 'runtime_transfer', 'runtime_prep', 'runtime_depth', 'cascade_lograt', 'cascade_blur', 'cascade_feed',
           'runtime_reuse_store', 'runtime_reuse_motion', 'runtime_reuse_warp', 'runtime_reuse_stat']
MOTION = ['motion_luma', 'motion_estimate']
# The sources whose pipelines are unrolled before they are compiled.
UNROLL = {'fswin_t.comp', 'attn.comp', 'ffwd3_t.comp'}


def glslang(arch, src, defines, out, unroll=False):
    include = '-I' + str(R / 'windows/shaders' / arch / 'include')
    tool = str(R / 'toolchain/glslang/bin/glslang')
    if unroll:
        pre = out.with_suffix('.pre.comp')
        r = subprocess.run([tool, '-E', include] + ['-D' + d for d in defines] + [str(src)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'glslang -E failed on {src}:\n{r.stdout}{r.stderr}')
        pre.write_text(r.stdout)
        subprocess.run([sys.executable, str(R / 'windows/build/unroll_glsl.py'), str(pre), str(pre)],
                       check=True, stdout=subprocess.DEVNULL)
        src, defines = pre, []
    cmd = [tool, '-V', '--target-env', 'vulkan1.3', include] + ['-D' + d for d in defines] + [str(src), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'glslang failed on {src}:\n{r.stdout}{r.stderr}')
    if unroll:
        src.unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arch', nargs='?', default='rdna3', choices=['rdna3'])
    ap.add_argument('--out', type=Path)
    ap.add_argument('--define', action='append', default=[],
                    help='extra define for every network pipeline (diagnostics, e.g. NR_PROF_OFF=...)')
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    passes = R / 'windows/shaders' / a.arch / 'passes'
    table = json.loads((R / 'windows/shaders' / a.arch / 'pipelines.json').read_text())
    out = a.out or R / 'build/windows' / a.arch / 'network'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    pipelines = table['pipelines']
    for name, e in pipelines.items():
        glslang(a.arch, R / 'windows/shaders' / a.arch / e['source'], e['defines'] + a.define, out / f'g_{name}.spv',
                unroll=e['source'] in UNROLL)
    for name, text in table['markers'].items():
        (out / name).write_text('\n'.join(text) + '\n' if isinstance(text, list) else text + '\n')
    (out / 'runtime').mkdir()
    (out / 'temporal').mkdir()
    for name, v in table['variants'].items():
        base = pipelines[v['base']]
        glslang(a.arch, R / 'windows/shaders' / a.arch / base['source'], base['defines'] + v['add'],
                out / 'temporal' / f'{name}.spv', unroll=base['source'] in UNROLL)
    for k in MOTION:
        glslang(a.arch, passes / f'{k}.comp', [], out / 'temporal' / f'{k}.spv')
    shutil.copy2(out / 'shader-constants.txt', out / 'temporal' / 'shader-constants.txt')
    for k in RUNTIME:
        glslang(a.arch, passes / f'{k}.comp', [], out / 'runtime' / f'{k}.spv')
    print(f'{out}: {len(pipelines)} network pipelines, {len(table["variants"]) + len(MOTION)} temporal, '
          f'{len(RUNTIME)} runtime')

    if a.check:
        bad = [p.name for p in sorted(out.iterdir()) if p.is_file()
               if not (a.check / p.name).is_file() or (a.check / p.name).read_bytes() != p.read_bytes()]
        print('check against', a.check, ':', 'identical' if not bad else 'DIFFERENT: ' + ' '.join(bad))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
