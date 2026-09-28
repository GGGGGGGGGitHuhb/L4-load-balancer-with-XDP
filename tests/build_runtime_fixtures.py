#!/usr/bin/env python3
"""Build S3 real-map test executable, faults and explicit coherence diagnostic."""
import argparse
from pathlib import Path
import subprocess as S

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--libbpf-prefix', type=Path, required=True)
parser.add_argument('--clang', default='clang-18')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
triplet = S.check_output(['cc', '-dumpmachine'], text=True).strip()
include = args.libbpf_prefix.resolve() / 'include'
lib = args.libbpf_prefix.resolve() / 'lib' / triplet
source = (root/'src/xdp/xdp_udp_runtime.bpf.c').read_text()
needle = '  __u32 hash = 2166136261U;'
assert source.count(needle) == 1
check = '''  /* Diagnostic only: generation, count, VIP and backend must belong to one publication. */
  if (config->vipAddress != __builtin_bswap32(0xc6130064U) ||
      config->vipPort != __builtin_bswap16(39001)) goto invalidConfig;
  if (config->generation & 1) {
    if (count != 1 || config->backends[0].ifindex != 11 ||
        config->backends[0].destinationMac[5] != 0xa1 ||
        config->backends[0].sourceMac[5] != 0xc1) goto invalidConfig;
  } else {
    if (count != 2 || config->backends[0].ifindex != 22 ||
        config->backends[1].ifindex != 22 ||
        config->backends[0].destinationMac[5] != 0xb2 ||
        config->backends[1].destinationMac[5] != 0xb2 ||
        config->backends[0].sourceMac[5] != 0xd2 ||
        config->backends[1].sourceMac[5] != 0xd2) goto invalidConfig;
  }
'''
fixture = out / 'runtime-diagnostic.bpf.c'
fixture.write_text(source.replace(needle, check + needle))
S.run(['clang-format', '-i', str(fixture)], check=True)
S.run([args.clang, '-target', 'bpf', '-O2', '-g', '-I'+str(root/'src/xdp'),
       '-I/usr/include/'+triplet, '-c', str(fixture), '-o', str(out/'runtime-diagnostic.bpf.o')], check=True)
S.run(['cc', '-shared', '-fPIC', '-I'+str(include), str(root/'tests/RuntimeMapFaults.c'),
       '-ldl', '-o', str(out/'libRuntimeMapFaults.so')], check=True)
S.run(['c++', '-std=c++20', '-pthread', '-I'+str(include), '-I'+str(root/'src'),
       str(root/'tests/RuntimeMapStoreKernel_test.cpp'), str(root/'src/xdp/RuntimeMapStore.cpp'),
       '-L'+str(lib), '-lbpf', '-o', str(out/'RuntimeMapStoreKernel_test')], check=True)
print('Built RuntimeMapStoreKernel_test, libRuntimeMapFaults.so, runtime-diagnostic.bpf.o')
