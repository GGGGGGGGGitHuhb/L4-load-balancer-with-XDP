"""Explicit, non-root XDP build matrix. Writes only to a new --work directory."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--clang', default=shutil.which('clang'))
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = work / 'source copy'
    source.mkdir()
    repo = Path(__file__).resolve().parents[1]
    for directory in ['src', 'cmake']:
        shutil.copytree(repo / directory, source / directory)
    shutil.copy2(repo / 'CMakeLists.txt', source / 'CMakeLists.txt')
    cxx = shutil.which('clang++') or shutil.which('g++')
    assert cxx and args.clang, 'C++ compiler and BPF Clang required'
    checks = []
    sequence = 0

    def run(label, command, fail=False, contains=None):
        nonlocal sequence
        sequence += 1
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=120)
        (work / f'{sequence:02d}-{label}.log').write_text(result.stdout)
        if (result.returncode != 0) != fail or (contains and contains not in result.stdout):
            raise RuntimeError(f'{label} unexpected result {result.returncode}; see {work}')
        return result.stdout

    def configure(name, *options, fail=False, contains=None, generator='Ninja'):
        build = work / name
        run(name, ['cmake', '-S', str(source), '-B', str(build), '-G', generator,
                   f'-DCMAKE_CXX_COMPILER={cxx}', '-DBUILD_TESTING=OFF',
                   '-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE', *options], fail, contains)
        return build

    off = configure('off', f'-DL4LB_BPF_CLANG={work}/absent')
    run('off-build', ['cmake', '--build', str(off), '-j2'])
    assert not (off / 'xdp/xdp_pass.bpf.o').exists()
    checks.append('OFF with missing BPF compiler; no Python or object')
    configure('missing', '-DL4LB_BUILD_XDP=ON', f'-DL4LB_BPF_CLANG={work}/absent',
              fail=True, contains='Clang not found')
    configure('wrong-compiler', '-DL4LB_BUILD_XDP=ON', '-DL4LB_BPF_CLANG=/bin/false',
              fail=True, contains='BPF compile probe failed')
    bad = work / 'bad headers'
    (bad / 'linux').mkdir(parents=True)
    (bad / 'linux/bpf.h').write_text('#error deliberate missing UAPI dependency\n')
    configure('bad-headers', '-DL4LB_BUILD_XDP=ON', f'-DL4LB_BPF_CLANG={args.clang}',
              f'-DL4LB_BPF_INCLUDE_DIRS={bad}', fail=True, contains='BPF compile probe failed')
    checks.append('ON rejects missing compiler, failing compiler, bad UAPI headers')
    on = configure('on with spaces', '-DL4LB_BUILD_XDP=ON', f'-DL4LB_BPF_CLANG={args.clang}')
    def build():
        return run('on-build', ['cmake', '--build', str(on), '--target', 'l4lb_xdp'])
    build()
    obj = on / 'xdp/xdp_pass.bpf.o'
    run('object', [sys.executable, str(repo / 'tests/xdp_object_test.py'), str(obj)])
    before = obj.stat().st_mtime_ns
    build()
    assert obj.stat().st_mtime_ns == before, 'no-op build recompiled object'
    copied = source / 'src/xdp/xdp_pass.bpf.c'
    copied.write_text(copied.read_text() + '\n// dependency rebuild\n')
    build()
    assert obj.stat().st_mtime_ns > before, 'source change not rebuilt'
    dependency = source / 'src/xdp/probe_dependency.h'
    dependency.write_text('// header dependency\n')
    copied.write_text('#include "probe_dependency.h"\n' + copied.read_text())
    build()
    before = obj.stat().st_mtime_ns
    dependency.write_text('// changed header dependency\n')
    build()
    assert obj.stat().st_mtime_ns > before, 'header change not rebuilt'
    checks.append('space paths, ELF/mutations, no-op, source and header dependencies')
    valid = copied.read_text()
    copied.write_text('#error deliberate build failure\n' + valid)
    run('failed-rebuild', ['cmake', '--build', str(on), '--target', 'l4lb_xdp'], fail=True)
    assert not obj.exists(), 'failed rebuild left a stale official object'
    copied.write_text(valid)
    build()
    run('clean', ['cmake', '--build', str(on), '--target', 'clean'])
    assert not obj.exists(), 'clean left object'
    build()
    checks.append('failed rebuild removes stale object, recovery and clean')
    make = configure('make', '-DL4LB_BUILD_XDP=ON', f'-DL4LB_BPF_CLANG={args.clang}',
                     generator='Unix Makefiles')
    run('make-build', ['cmake', '--build', str(make), '--target', 'l4lb_xdp'])
    run('make-object', [sys.executable, str(repo / 'tests/xdp_object_test.py'), str(make / 'xdp/xdp_pass.bpf.o')])
    checks.append('Unix Makefiles')
    (work / 'summary.txt').write_text('\n'.join('PASS: ' + item for item in checks) + '\n')
    print((work / 'summary.txt').read_text(), end='')


if __name__ == '__main__':
    main()
