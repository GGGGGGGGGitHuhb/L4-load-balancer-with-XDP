#!/usr/bin/env python3
"""Validate profile boundaries and malformed objects before any BPF syscall."""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--loader', required=True)
    parser.add_argument('--validator', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--source', required=True)
    args = parser.parse_args()
    base = ['attach', '--dev', 'lo', '--object', 'missing']
    cases = [
        ['--udp-dsr'], ['--udp-dsr', '--maps'], ['--maps', '--udp-dsr'],
        ['--udp-dsr', '--udp-dsr'], ['--vip', '10.0.0.1:90'],
        ['--target', 'eth0@02:00:00:00:00:01'],
        ['--udp-dsr', '--vip', '10.0.0.1:90', '--backend', '10.0.0.2:90'],
        ['--udp-dsr', '--vip', '10.0.0.1:90', '--vip', '10.0.0.2:90'],
    ]
    for vip in ['', '0.0.0.0:90', '224.0.0.1:90', '255.255.255.255:90', 'localhost:90', '1.2.3.4:0', '1.2.3.4:65536']:
        cases.append(['--udp-dsr', '--vip', vip])
    for target in ['eth0', 'eth0@00:00:00:00:00:00', 'eth0@ff:ff:ff:ff:ff:ff', 'eth0@03:00:00:00:00:01', 'eth0@02:00:00:00:00:gg']:
        cases.append(['--udp-dsr', '--vip', '10.0.0.1:90', '--target', target])
    commands = [base + options for options in cases]
    for options in [['--udp-dsr'], ['--vip', '10.0.0.1:90'], ['--target', 'eth0@02:00:00:00:00:01']]:
        commands.append(['detach', '--dev', 'lo', '--prog-id', '1'] + options)
    for command in commands:
        result = subprocess.run([args.loader, *command], capture_output=True, text=True, timeout=5)
        assert result.returncode == 2 and 'READY' not in result.stdout, (command, result.returncode, result.stdout, result.stderr)
    source = pathlib.Path(args.source)
    original = source.read_text()
    mutations = {
        'name': original.replace('l4lb_be_v2', 'wrong_be'),
        'type': original.replace('L4LB_UINT(type, BPF_MAP_TYPE_ARRAY)', 'L4LB_UINT(type, BPF_MAP_TYPE_HASH)', 1),
        'key': original.replace('L4LB_TYPE(key, __u32)', 'L4LB_TYPE(key, __u64)', 1),
        'value': original.replace('L4LB_TYPE(value, struct UdpDsrConfigValue)', 'L4LB_TYPE(value, __u32)', 1),
        'capacity': original.replace('L4LB_UINT(max_entries, 64)', 'L4LB_UINT(max_entries, 63)'),
        'flags': original.replace('L4LB_UINT(map_flags, BPF_F_RDONLY_PROG)', 'L4LB_UINT(map_flags, 0)', 1),
        'missing': original.replace('l4lb_be_v2 SEC(".maps")', 'l4lb_be_v2'),
        'extra': original + '\nstruct { L4LB_UINT(type, BPF_MAP_TYPE_ARRAY); L4LB_UINT(max_entries, 1); L4LB_TYPE(key, __u32); L4LB_TYPE(value, __u32); } extra SEC(".maps");\n',
        'global': original.replace('SEC("xdp")', 'volatile int extraGlobal;\nSEC("xdp")').replace('__u32 key = 0;', '__u32 key = 0; extraGlobal++;'),
        'extra_program': original + '\nSEC("xdp") int other(struct xdp_md *ctx) { (void)ctx; return XDP_PASS; }\n',
        'program': original.replace('xdp_udp_dsr(', 'wrong_program('),
        'section': original.replace('SEC("xdp")', 'SEC("socket")'),
    }
    with tempfile.TemporaryDirectory(prefix='dsr-cli-') as directory:
        for name, contents in mutations.items():
            fixture = pathlib.Path(directory) / (name + '.c')
            obj = fixture.with_suffix('.o')
            fixture.write_text(contents)
            subprocess.run([args.clang, '-target', 'bpf', '-O2', '-g', '-I', str(source.parent), '-I', '/usr/include/x86_64-linux-gnu', '-c', str(fixture), '-o', str(obj)], capture_output=True, check=True, timeout=10)
            result = subprocess.run([args.validator, '--reject', str(obj)], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
    print(f'DSR CLI {len(commands)} invalid combinations and {len(mutations)} malformed objects PASS')


if __name__ == '__main__':
    main()
