#!/usr/bin/env python3
"""Build explicit fault fixtures, preserving the production object and source."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--clang', default='clang-18')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
args.output.mkdir(parents=True, exist_ok=True)
source = (root/'src/xdp/xdp_udp_dsr.bpf.c').read_text()
needle = 'redirectPacket(backend->ifindex, 0)'
assert source.count(needle) == 1, 'fixture must track one explicit production helper call'
fixture = args.output/'helper-error.bpf.c'
fixture.write_text(source.replace(needle, 'redirectPacket(backend->ifindex, 1)'))
subprocess.run([args.clang, '-target', 'bpf', '-O2', '-g', '-I'+str(root/'src/xdp'),
                '-I/usr/include/' + subprocess.check_output(['cc', '-dumpmachine'], text=True).strip(), '-c', str(fixture), '-o',
                str(args.output/'helper-error.bpf.o')], check=True)
subprocess.run(['cc', '-shared', '-fPIC', str(root/'tests/XdpDsrFaults.c'), '-ldl', '-o',
                str(args.output/'libdsr-faults.so')], check=True)
print('Built helper-error.bpf.o and libdsr-faults.so')

# Linux test-run requires Ethernet-sized buffers: crop the parser's logical bound
# in an explicitly separate fixture, without pretending these are short wire frames.
needle = '__u8 *end = (void *)(long)ctx->data_end;'
assert source.count(needle) == 1
short = args.output/'short-bound.bpf.c'
short.write_text(source.replace(needle, needle + '\n  if (data + 42 > end) goto unsupported;\n  if (data[0] < 14) end = data + data[0];'))
subprocess.run([args.clang, '-target', 'bpf', '-O2', '-g', '-I'+str(root/'src/xdp'),
                '-I/usr/include/' + subprocess.check_output(['cc', '-dumpmachine'], text=True).strip(),
                '-c', str(short), '-o', str(args.output/'short-bound.bpf.o')], check=True)
