#!/usr/bin/env python3
"""Reject malformed runtime CLI and BTF objects without loading BPF."""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    for name in ['loader', 'validator', 'source', 'object', 'clang']:
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    base = [args.loader, 'attach', '--dev', 'lo', '--object', args.object]
    cases = [
        ['--udp-dsr-runtime'],
        ['--runtime-config', 'config'],
        ['--udp-dsr-runtime', '--vip', '10.0.0.1:9'],
        ['--udp-dsr-runtime', '--runtime-config', 'config'],
        ['--udp-dsr-runtime', '--maps'],
        ['--maps', '--udp-dsr-runtime'],
        ['--udp-dsr-runtime', '--udp-dsr'],
        ['--udp-dsr-runtime', '--udp-dsr-runtime'],
        ['--udp-dsr-runtime', '--vip', '10.0.0.1:9', '--runtime-config', 'config', '--target', 'e@02:00:00:00:00:01'],
        ['--udp-dsr-runtime', '--vip', '10.0.0.1:9', '--runtime-config', 'config', '--backend', '10.0.0.2:9'],
        ['--udp-dsr', '--vip', '10.0.0.1:9', '--runtime-config', 'config'],
        ['--udp-dsr-runtime', '--vip', '10.0.0.1:9', '--runtime-config', 'config', '--runtime-config', 'other'],
    ]
    for vip in ['0.0.0.0:9', '224.0.0.1:9', 'localhost:9', '1.2.3.4:0', '1.2.3.4:65536']:
        cases.append(['--udp-dsr-runtime', '--vip', vip, '--runtime-config', 'config'])
    for options in cases:
        result = subprocess.run(base + options, capture_output=True, text=True, timeout=5)
        assert result.returncode == 2 and 'READY ' not in result.stdout, (options, result)
        assert '无法打开 runtime' not in result.stderr, 'CLI rejection masked by missing config'
    subprocess.run([args.validator, '--accept', args.object], check=True, capture_output=True)
    source = pathlib.Path(args.source)
    original = source.read_text()
    mutations = {
        'name': original.replace('l4lb_stats_v3', 'wrong_stats'),
        'inner_type': original.replace('BPF_MAP_TYPE_ARRAY);', 'BPF_MAP_TYPE_HASH);', 1),
        'outer_type': original.replace('BPF_MAP_TYPE_ARRAY_OF_MAPS', 'BPF_MAP_TYPE_HASH_OF_MAPS'),
        'inner_key': original.replace('L4LB_TYPE(key, __u32)', 'L4LB_TYPE(key, __u64)', 1),
        'inner_value': original.replace('(*value)[1]', '(*value)[2]', 1),
        'capacity': original.replace('L4LB_UINT(max_entries, 1)', 'L4LB_UINT(max_entries, 2)', 1),
        'flags': original.replace('BPF_F_RDONLY_PROG', '0', 1),
        'extra': original + '\nstruct { L4LB_UINT(type, BPF_MAP_TYPE_ARRAY); L4LB_UINT(max_entries, 1); L4LB_TYPE(key, __u32); L4LB_TYPE(value, __u32); } extra SEC(".maps");\n',
        'program': original.replace('xdp_udp_rt(', 'wrong_program('),
        'extra_program': original + '\nSEC("xdp") int other(struct xdp_md *ctx) { (void)ctx; return XDP_PASS; }\n',
        'section': original.replace('SEC("xdp")', 'SEC("socket")'),
    }
    architecture = subprocess.check_output(['cc', '-dumpmachine'], text=True).strip()
    with tempfile.TemporaryDirectory(prefix='runtime-cli-') as directory:
        for name, contents in mutations.items():
            assert contents != original, name
            fixture = pathlib.Path(directory) / (name + '.c')
            obj = fixture.with_suffix('.o')
            fixture.write_text(contents)
            subprocess.run([args.clang, '-target', 'bpf', '-O2', '-g', '-I', str(source.parent), '-I/usr/include/' + architecture, '-c', str(fixture), '-o', str(obj)], capture_output=True, check=True, timeout=10)
            result = subprocess.run([args.validator, '--reject', str(obj)], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
    print(f'Runtime CLI {len(cases)} and malformed objects {len(mutations)} PASS')


if __name__ == '__main__':
    main()
