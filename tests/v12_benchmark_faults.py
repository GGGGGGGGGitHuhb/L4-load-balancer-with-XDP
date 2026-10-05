#!/usr/bin/env python3
"""Real short-run failure proofs, independent of the 126 formal measurements."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess as S
import sys
import time

from v12_benchmark import environment, run_one, tools_identity, verify_identity, write
from v12_benchmark_data import digest, require


def host_network():
    return {kind: json.loads(S.check_output(['ip', '-j', kind], text=True)) for kind in ['address', 'route']}


def verify_failure(path, fault):
    run = json.loads((path / 'run.json').read_text())
    supervisor = json.loads((path / 'supervisor.json').read_text())
    require(not run['valid'] and run['error'], 'failure must be primary')
    require(not run['cleanup_errors'] and run['cleanup']['owned_reaped'] and not run['cleanup']['remaining'], 'failed cleanup')
    require(supervisor['owned_reaped'] and not supervisor['remaining_group'], 'outer failed cleanup')
    require((path / 'traffic.started').exists(), 'fault before traffic')
    require(sum(row.get('measurement_received', 0) for row in run['backends']) > 0, 'fault before backend traffic')
    logs = '\n'.join(p.read_text() for p in path.glob('process-*.log'))
    expected = {'corrupt': 'corrupt/cross-client reply', 'cross-client': 'corrupt/cross-client reply',
                'child-exit': 'ready child exited', 'sampler': 'injected sampler read failure', 'write': 'injected evidence write failure',
                'watchdog': 'signal termination', 'sigterm': 'signal termination'}[fault]
    require(expected in run['error'] + logs, 'wrong fault detected: ' + fault)
    if fault == 'watchdog':
        require(supervisor['error'] == 'watchdog deadline', 'watchdog not fired')
    return dict(fault=fault, error=run['error'], elapsed_s=supervisor['elapsed_s'], cleanup=run['cleanup'],
                backend_received=sum(row.get('measurement_received', 0) for row in run['backends']))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--product', type=Path, required=True)
    p.add_argument('--directory', type=Path, required=True)
    args = p.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    before = host_network()
    mount_before = Path('/proc/self/mountinfo').read_text()
    identity = json.loads(args.product.read_text())
    identity['tools'] = tools_identity()
    verify_identity(identity)
    spec = dict(path='runtime-generic', payload=64, rate=1000, repeat=0, scene='steady', kind='data', run_id='fault', clients=16,
                warmup_s=.2, window_s=1, timeout_s=1)
    rows = []
    for fault in ['corrupt', 'cross-client', 'child-exit', 'sampler', 'write', 'watchdog']:
        path = directory / fault
        try:
            run_one(path, identity, spec, fault, watchdog=7 if fault == 'watchdog' else 45)
        except RuntimeError:
            rows.append(verify_failure(path, fault))
        else:
            raise AssertionError('fault was not detected: ' + fault)
    # Signal the outer runner only after its generator has actually sent data.
    path = directory / 'outer-sigterm'
    command = [sys.executable, str(Path(__file__).with_name('v12_benchmark.py')), 'run', '--short', '--short-window', '3',
               '--paths', 'runtime-native', '--product', str(args.product.resolve()), '--directory', str(path)]
    with (directory / 'sigterm.log').open('w') as log:
        process = S.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while not (path / '000/traffic.started').exists():
                require(process.poll() is None and time.monotonic() < deadline, 'SIGTERM setup failed')
                time.sleep(.01)
            time.sleep(.1)
            process.send_signal(signal.SIGTERM)
            require(process.wait(timeout=8) != 0, 'outer SIGTERM exit')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    rows.append(verify_failure(path / '000', 'sigterm'))
    wrong = json.loads(json.dumps(identity))
    wrong['binaries']['proxy']['sha256'] = '0' * 64
    try:
        verify_identity(wrong)
    except ValueError as exc:
        require('wrong binary identity' in str(exc), 'identity fault reason')
        rows.append(dict(fault='identity', error=str(exc), namespace_created=False))
    else:
        raise AssertionError('wrong identity accepted')
    require(host_network() == before, 'host network changed')
    require(Path('/proc/self/mountinfo').read_text() == mount_before, 'host mount changed')
    write(directory / 'summary.json', dict(environment=environment(), identity_sha256=digest(identity), tests=rows,
                                           host_network_unchanged=True, host_mounts_unchanged=True))
    print(json.dumps(dict(passed=len(rows), directory=str(directory))))


if __name__ == '__main__':
    main()
