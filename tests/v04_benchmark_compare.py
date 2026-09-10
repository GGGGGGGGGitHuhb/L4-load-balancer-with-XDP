#!/usr/bin/env python3
"""Fixed S1/S2 build, serial comparison, and portable offline recomputation."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from v04_benchmark_identity import ROOT, VERSIONS, digest, prepare, require, tools_identity, verify, write
from v04_benchmark_data import load_package, parameters, save_package, schedule, summarize


def run(output, builds, smoke=False, fault='none', override=None):
    output.mkdir(parents=True, exist_ok=False)
    manifests = {role: json.loads((builds / role / 'manifest.json').read_text()) for role in VERSIONS}
    tools = tools_identity()
    data = dict(schema=1, smoke=smoke, schedule=schedule(smoke), manifests=manifests,
                tool_sha256=tools, runs=[])
    failure_primary = None
    failure_cleanup = []
    write(output / 'schedule.json', data['schedule'])
    try:
        for role, manifest in manifests.items():
            verify(manifest, override if role == 'candidate' else None)
        require(manifests['baseline']['versions'] == manifests['candidate']['versions'], 'build tool mismatch')
        write(output / 'validated-builds.json', {'manifests': manifests, 'tool_sha256': tools})
        for index, cell in enumerate(data['schedule']):
            require(tools_identity() == tools, 'tools changed during batch')
            manifest = manifests[cell['role']]
            verify(manifest)
            identity = {'role': cell['role'], 'manifest_sha256': digest(manifest), 'tool_sha256': tools}
            # Keep provenance even if the runner fails before writing its result.
            write(output / f'{index:02d}.product_identity.json', identity)
            directory = output / f'{index:02d}-{cell["role"]}-{cell["protocol"]}-{cell["clients"]}-{cell["mode"]}'
            params = parameters(cell, smoke)
            if fault != 'none' and cell['mode'] == 'proxy':
                params['fault'] = fault
            argv = [sys.executable, str(ROOT / 'tests/benchmark_runner.py'), '--program', manifest['binary_path'],
                    '--output', str(directory)]
            for key, value in params.items():
                argv.extend(['--' + key, str(value)])
            invocation = {'argv': argv, 'start_ns': time.monotonic_ns()}
            with (output / f'{index:02d}.stdout.log').open('w') as out, (output / f'{index:02d}.stderr.log').open('w') as err:
                child = subprocess.Popen(argv, stdout=out, stderr=err)
                invocation['pid'] = child.pid
                try:
                    code = child.wait(timeout=60)
                except BaseException:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                    raise
                finally:
                    invocation.update(returncode=child.returncode, reaped=child.poll() is not None,
                                      end_ns=time.monotonic_ns())
                    write(output / f'{index:02d}.invocation.json', invocation)
            paths = list(directory.glob('*/result.json'))
            require(len(paths) == 1, 'missing runner result')
            result_path = paths[0]
            r = json.loads(result_path.read_text())
            require(all(p['reaped'] and not Path(f'/proc/{p["pid"]}').exists() for p in r['owned_processes']), 'child resource reap')
            if code != 0 or not r['valid']:
                failure_primary = r['primary_error'] or f'runner invalid or exit {code}'
                failure_cleanup = r['cleanup_errors']
                raise RuntimeError('runner failed; see original result and failure.json')
            record = {'cell': cell, 'parameters': params, 'invocation': invocation, 'result': r,
                      'product_identity': identity,
                      'environment': json.loads((result_path.parent / 'environment.json').read_text()),
                      'configuration': (result_path.parent / 'configuration.txt').read_text(),
                      'rtt': [json.loads(line) for line in (result_path.parent / 'rtt-samples.jsonl').read_text().splitlines()],
                      'resources': json.loads((result_path.parent / 'resource-samples.json').read_text())}
            write(result_path.parent / 'product_identity.json', record['product_identity'])
            data['runs'].append(record)
            print(f'{index + 1}/{len(data["schedule"])} {cell} valid', flush=True)
        require(tools_identity() == tools, 'final tools changed')
        summary = summarize(data)
        write(output / 'summary.json', summary)
        # Normalize paths only, never numeric samples. Manifest digests must be rebound.
        portable = json.loads(json.dumps(data).replace(str(ROOT), '$REPO'))
        for record in portable['runs']:
            record['product_identity']['manifest_sha256'] = digest(portable['manifests'][record['cell']['role']])
        summarize(portable)
        save_package(output / 'raw.json.gz', portable)
        write(output / 'portable-summary.json', summarize(load_package(output / 'raw.json.gz')))
        return data
    except BaseException as error:
        write(output / 'failure.json', {'valid': False,
                                       'primary_error': failure_primary or f'{type(error).__name__}: {error}',
                                       'cleanup_errors': failure_cleanup, 'completed_runs': len(data['runs'])})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('build')
    build.add_argument('--output', type=Path, required=True)
    measure = sub.add_parser('run')
    measure.add_argument('--builds', type=Path, required=True)
    measure.add_argument('--output', type=Path, required=True)
    measure.add_argument('--smoke', action='store_true', help='4 short TCP runs, never a formal report')
    measure.add_argument('--fault', choices=['none', 'product-exit', 'background', 'cleanup'], default='none', help=argparse.SUPPRESS)
    measure.add_argument('--candidate-program', type=Path, help=argparse.SUPPRESS)
    recalc = sub.add_parser('recompute')
    recalc.add_argument('--package', type=Path, required=True)
    recalc.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'build':
            prepare(args.output.resolve())
        elif args.command == 'run':
            require(args.fault == 'none' or args.smoke, 'faults require nonformal smoke')
            run(args.output.resolve(), args.builds.resolve(), args.smoke, args.fault, args.candidate_program)
        else:
            args.output.mkdir(parents=True, exist_ok=False)
            write(args.output / 'summary.json', summarize(load_package(args.package)))
        return 0
    except Exception as error:
        print(f'{type(error).__name__}: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
