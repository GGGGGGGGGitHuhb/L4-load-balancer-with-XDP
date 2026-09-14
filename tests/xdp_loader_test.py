#!/usr/bin/env python3
"""Unprivileged CLI contract tests; never attach to a real interface."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--clang', default='clang')
    args = parser.parse_args()
    count = 0

    def check(arguments, code, text):
        nonlocal count
        result = subprocess.run([str(args.loader), *arguments], capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == code, (arguments, result.returncode, result.stderr)
        assert text in result.stdout + result.stderr, (arguments, result.stdout, result.stderr)
        count += 1

    check(['--help'], 0, 'SIGINT/SIGTERM')
    for arguments in [[], ['status'], ['attach'], ['detach', '--dev', 'lo'],
                      ['attach', '--dev', 'lo', '--object', 'x', '--dev', 'lo'],
                      ['attach', '--dev', 'lo', '--object', 'x', '--mode', 'auto'],
                      ['attach', '--dev', 'lo', '--object', 'x', '--mode', 'generic', '--mode', 'native'],
                      ['attach', '--dev', 'x' * 16, '--object', 'x'],
                      ['attach', '--dev', 'bad/name', '--object', 'x'],
                      ['attach', '--dev', 'lo', '--object', 'x', '--prog-id', '1'],
                      ['detach', '--dev', 'lo', '--prog-id', '1', '--object', 'x'],
                      ['detach', '--dev', 'lo', '--prog-id', '1', '--prog-id', '2'],
                      ['--help', 'extra']]:
        check(arguments, 2, '参数错误')
    for bad in ['0', '-1', '+1', '4294967296', '1x', '', ' 1']:
        check(['detach', '--dev', 'lo', '--prog-id', bad], 2, '参数错误')
    check(['attach', '--dev', 'xdp-no-such', '--object', str(args.object)], 1, '找不到网络设备')
    with tempfile.TemporaryDirectory(prefix='l4lb-xdp-cli-') as directory:
        root = Path(directory)
        base = ['attach', '--dev', 'lo', '--object']
        check(base + [str(root / 'absent')], 1, '打开对象')
        check(base + [str(root)], 1, '普通文件')
        fifo = root / 'fifo'
        os.mkfifo(fifo)
        check(base + [str(fifo)], 1, '普通文件')
        bad = root / 'bad.o'
        bad.write_bytes(b'not an ELF object')
        check(base + [str(bad)], 1, '解析 BPF 对象')
        bad.write_bytes(b'')
        check(base + [str(bad)], 1, '对象大小')
        with bad.open('wb') as output:
            output.truncate(16 * 1024 * 1024 + 1)
        check(base + [str(bad)], 1, '对象大小')
        # Corrupt the program name without changing string-table size.
        data = args.object.read_bytes()
        assert b'xdp_pass\0' in data
        bad.write_bytes(data.replace(b'xdp_pass\0', b'bad_pass\0'))
        check(base + [str(bad)], 1, '对象应仅包含')
        source = '#define SEC(s) __attribute__((section(s), used))\n'
        source += 'SEC("xdp") int xdp_pass(void *ctx) { return 2; }\n'
        source += 'SEC("license") char LICENSE[] = "GPL";\n'
        variants = {
            'extra-program': source + 'SEC("xdp/extra") int extra(void *ctx) { return 2; }\n',
            'map': source + 'struct { int (*type)[2]; int (*max_entries)[1]; int *key; '
                            'int *value; } settings SEC(".maps");\n',
            'wrong-section': source.replace('SEC("xdp")', 'SEC("socket")'),
        }
        for name, contents in variants.items():
            fixture = root / (name + '.c')
            fixture.write_text(contents)
            built = subprocess.run([args.clang, '-target', 'bpf', '-O2', '-g', '-c',
                                    str(fixture), '-o', str(bad)], capture_output=True,
                                   text=True, timeout=10)
            assert built.returncode == 0, built.stderr
            check(base + [str(bad)], 1, '对象应仅包含')
        # This test must run as an ordinary UID without BPF administration caps.
        assert os.geteuid() != 0, 'run xdp_loader_cli as ordinary UID, outside root user namespace'
        check(base + [str(args.object)], 1, 'CAP_BPF/CAP_NET_ADMIN')
    print(f'PASS: {count} unprivileged loader CLI checks')


if __name__ == '__main__':
    main()
