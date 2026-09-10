#!/usr/bin/env python3
"""Small deterministic oracle, full-grid validation and opt-in real failures."""
import argparse
import concurrent.futures
import copy
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from v04_benchmark_data import delta, load_package, parameters, ranks, raw_metrics, save_package, schedule, summarize
from v04_benchmark_identity import ROOT, VERSIONS, digest, require, source_files, static_manifest, write


def fixture():
    manifests = {role: {'role': role, 'binary_sha256': role, 'versions': {}, 'effective_flags': {}, 'compiler_path': '/compiler'} for role in VERSIONS}
    data = dict(schema=1, smoke=False, schedule=schedule(), manifests=manifests, tool_sha256={'test': 'hash'}, runs=[])
    for index, cell in enumerate(schedule()):
        params = parameters(cell)
        counts = dict(sent=100, received=100, attempted=100, send_errors=0, timeout=0, foreign_corrupt=0,
                      target=50000, missed_slots=49900 if cell['protocol'] == 'udp' else 0, duplicate=0, late=0)
        r = dict(valid=True, exit_code=0, primary_error=None, cleanup_errors=[], run_id=str(index),
                 mode=cell['mode'], protocol=cell['protocol'], counts=counts,
                 fixture_fd={'before': 4, 'after': 4}, backend_cleanup={'fd_before': 4, 'fd_after': 4},
                 owned_processes=[{'reaped': True}],
                 timing=dict(t0_ns=1000000000, t1_ns=6000000000, drain_end_ns=6000000000,
                             send_window_seconds=5, goodput_elapsed_seconds=5),
                 rates=dict(sent_per_second=20, unique_echoes_per_second=20, goodput_bytes_per_second=20 * params['payload'],
                            goodput_bits_per_second=160 * params['payload'], delivery_ratio=1, unconfirmed=0, loss_ratio=0),
                 under_target=cell['protocol'] == 'udp',
                 sampled_rtt=dict(count=1, stride=100, coverage=.01, p99_insufficient_samples=True,
                                  p50_ns=100, p95_ns=100, p99_ns=100, max_ns=100), resources={})
        resources = {}
        for name in ['generator', 'backend'] + (['product'] if cell['mode'] == 'proxy' else []):
            resources[name] = [dict(monotonic_ns=1000000000, cpu_seconds=1, rss_bytes=100, fd_count=4),
                               dict(monotonic_ns=6000000000, cpu_seconds=2, rss_bytes=200, fd_count=5)]
            r['resources'][name] = dict(start_ns=1000000000, end_ns=6000000000, wall_seconds=5,
                                       cpu_seconds=1, cpu_percent_one_core=20, sampled_peak_rss_bytes=200,
                                       sampled_peak_fd=5, sample_count=2)
        if cell['mode'] == 'direct':
            r['resources']['product'] = {'value': None}
        data['runs'].append(dict(cell=cell, parameters=params, result=r, resources=resources,
                                configuration='health_check=off\nmetrics=off\n', rtt=[dict(client=0, seq=0, rtt_ns=100)],
                                invocation=dict(returncode=0, start_ns=index * 10 + 1, end_ns=index * 10 + 2),
                                product_identity=dict(role=cell['role'], manifest_sha256=digest(manifests[cell['role']]), tool_sha256=data['tool_sha256']),
                                environment=dict(binary_sha256={'value': cell['role']}, parameters=params, tool_sha256={'test': 'hash'})))
    return data


class ComparisonTests(unittest.TestCase):
    def test_known_arithmetic(self):
        record = fixture()['runs'][1]
        values = raw_metrics(record)
        self.assertEqual(values['goodput_bytes_per_second'], 81920)
        self.assertEqual(values['product_cpu_percent'], 20)
        self.assertEqual(values['product_rss_bytes'], 200)
        self.assertEqual(ranks(list(range(1, 101)))['p99_ns'], 99)
        self.assertIsNone(ranks([])['p99_ns'])
        self.assertIsNone(delta(0, 1)['relative_percent'])
        self.assertEqual(delta(10, 12)['relative_percent'], 20)

    def test_grid_and_mutations(self):
        original = fixture()
        with patch('v04_benchmark_data.static_manifest'):
            self.assertEqual(len(summarize(original)['runs']), 48)
            mutations = [lambda d: d['runs'].pop(),
                         lambda d: d['runs'].append(d['runs'][0]),
                         lambda d: d['runs'][1].update(cell=d['runs'][0]['cell']),
                         lambda d: d['runs'][0]['result'].update(valid=False),
                         lambda d: d['runs'][0]['parameters'].update(payload=99),
                         lambda d: d['runs'][0]['product_identity'].update(role='candidate'),
                         lambda d: d['runs'][0]['environment']['tool_sha256'].update(test='changed'),
                         lambda d: d['runs'][0]['result']['counts'].update(received=99),
                         lambda d: d['runs'][0]['rtt'][0].update(rtt_ns=101),
                         lambda d: d['runs'][0]['resources']['generator'][1].update(cpu_seconds=3),
                         lambda d: d['runs'][1]['invocation'].update(start_ns=0)]
            for mutation in mutations:
                with self.subTest(mutation=mutation):
                    changed = copy.deepcopy(original)
                    mutation(changed)
                    with self.assertRaises(ValueError):
                        summarize(changed)

    def test_package_sha_and_manifest_ref(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'raw.gz'
            data = fixture()
            save_package(path, data)
            self.assertEqual(load_package(path), data)
            envelope = json.loads(gzip.decompress(path.read_bytes()))
            envelope['payload']['runs'][0]['rtt'][0]['rtt_ns'] += 1
            path.write_bytes(gzip.compress(json.dumps(envelope).encode()))
            with self.assertRaisesRegex(ValueError, 'SHA'):
                load_package(path)
        with self.assertRaisesRegex(ValueError, 'ref mismatch'):
            static_manifest(dict(schema=1, role='baseline', ref='v0.4-s2', commit=VERSIONS['baseline'][1]))
        self.assertNotEqual(source_files(VERSIONS['baseline'][1]), source_files(VERSIONS['candidate'][1]))


def acceptance(builds, output, package=None):
    output.mkdir(parents=True, exist_ok=False)
    records = []
    command = [sys.executable, str(ROOT / 'tests/v04_benchmark_compare.py')]

    def execute(name, args, expected=0):
        argv = command + args + ['--output', str(output / name)]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=90)
        (output / (name.replace('/', '_') + '.log')).write_text(result.stdout + result.stderr)
        record = dict(name=name, argv=argv, returncode=result.returncode, expected=expected)
        records.append(record)
        write(output / (name.replace('/', '_') + '.invocation.json'), record)
        require(result.returncode == expected, 'unexpected returncode: ' + name)
        return output / name

    run_args = ['run', '--builds', str(builds), '--smoke']
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(execute, 'parallel-' + str(i), run_args) for i in range(2)]
        for future in futures:
            future.result()
    execute('output with spaces', run_args)
    wrong = execute('wrong-binary', run_args + ['--candidate-program', str(builds / 'baseline/build/bin/l4lb')], 1)
    require(not list(wrong.glob('*.invocation.json')), 'wrong identity launched traffic')
    for fault in ['product-exit', 'background', 'cleanup']:
        bad = execute('fault-' + fault, run_args + ['--fault', fault], 1)
        results = [json.loads(p.read_text()) for p in bad.glob('*/*/result.json')]
        failed = [r for r in results if not r['valid']]
        require(failed and any(r['owned_processes'] for r in failed), 'failure must follow launch')
        outer = json.loads((bad / 'failure.json').read_text())
        require(outer['cleanup_errors'] == failed[-1]['cleanup_errors'], 'outer cleanup must remain separate')
        if failed[-1]['primary_error']:
            require(outer['primary_error'] == failed[-1]['primary_error'], 'outer primary must be preserved')
        verified = json.loads((bad / 'validated-builds.json').read_text())
        for path in bad.glob('*.invocation.json'):
            identity = json.loads(path.with_name(path.name.replace('.invocation', '.product_identity')).read_text())
            require(identity['manifest_sha256'] == digest(verified['manifests'][identity['role']]), 'failed-run manifest identity')
            require(identity['tool_sha256'] == verified['tool_sha256'], 'failed-run tool identity')
        for r in results:
            require(r['fixture_fd']['before'] == r['fixture_fd']['after'], 'negative fd leak')
            for process in r['owned_processes']:
                require(process['reaped'] and not Path('/proc/' + str(process['pid'])).exists(), 'negative PID not reaped')
    original = load_package(package or output / 'parallel-0/raw.json.gz')
    for name in ['missing', 'count', 'rtt', 'identity']:
        changed = copy.deepcopy(original)
        if name == 'missing':
            changed['runs'].pop()
        elif name == 'count':
            changed['runs'][0]['result']['counts']['received'] -= 1
        elif name == 'rtt':
            changed['runs'][0]['rtt'][0]['rtt_ns'] += 1000000000
        else:
            changed['runs'][0]['product_identity']['manifest_sha256'] = '0' * 64
        path = output / (name + '.json.gz')
        save_package(path, changed)  # Fresh package hash must not mask semantic corruption.
        execute('reject-' + name, ['recompute', '--package', str(path)], 1)
    write(output / 'invocations.json', records)
    print(json.dumps({'calls': len(records), 'valid': True, 'output': str(output)}))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'acceptance':
        parser = argparse.ArgumentParser()
        parser.add_argument('acceptance')
        parser.add_argument('--builds', type=Path, required=True)
        parser.add_argument('--output', type=Path, required=True)
        parser.add_argument('--package', type=Path)
        args = parser.parse_args()
        acceptance(args.builds.resolve(), args.output.resolve(), args.package)
    else:
        unittest.main()
