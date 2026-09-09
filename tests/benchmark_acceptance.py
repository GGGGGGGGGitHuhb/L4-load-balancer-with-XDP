#!/usr/bin/env python3
"""Opt-in acceptance matrix; never part of the default long-running CTest suite."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from benchmark_tool_test import verify


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', choices=['matrix', 'failure', 'udp-check', 'isolation', 'permission'], required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    runner = Path(__file__).with_name('benchmark_runner.py')

    def execute(name, protocol='tcp', extra=(), fail=False, interrupt=False):
        output = args.output / name
        command = [sys.executable, str(runner), '--program', str(args.program.resolve()),
                   '--protocol', protocol, '--output', str(output), *extra]
        if interrupt:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 10
                while not any('服务已启动' in p.read_text() for p in output.glob('*/product-*.stdout.log')):
                    assert child.poll() is None and time.monotonic() < deadline, 'interrupt ready failed'
                    time.sleep(.01)
                child.terminate()
                stdout, stderr = child.communicate(timeout=40)
                completed = subprocess.CompletedProcess(command, child.returncode, stdout, stderr)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate()
        else:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
        (args.output / f'{name}.stdout.log').write_text(completed.stdout)
        (args.output / f'{name}.stderr.log').write_text(completed.stderr)
        record = {'name': name, 'argv': command, 'returncode': completed.returncode,
                  'expected_failure': fail, 'uid': os.getuid()}
        records.append(record)
        (args.output / f'{name}.invocation.json').write_text(json.dumps(record, indent=2))
        assert (completed.returncode != 0) == fail, record
        if fail:
            summary = json.loads((output / 'summary.json').read_text())
            assert not summary['valid']
            path = output / summary['runs'][0]['path']
            result = json.loads((path / ('result.json' if (path / 'result.json').is_file() else 'failure.json')).read_text())
            assert result['primary_error'] or result['cleanup_errors']
            assert result['owned_processes'], 'must fail after process launch'
            assert result['fixture_fd']['before'] == result['fixture_fd']['after']
            for process in result['owned_processes']:
                assert process['reaped'] and not Path(f"/proc/{process['pid']}").exists(), process
            if 'backend_cleanup' in result:
                assert result['backend_cleanup']['fd_before'] == result['backend_cleanup']['fd_after']
        else:
            verify(output)
        return output

    short = ['--duration', '1', '--warmup', '0', '--repeats', '1']

    def udp_checks():
        execute('udp-corrupt', 'udp', [*short, '--mode', 'proxy', '--fault', 'corrupt'], True)
        cross = execute('udp-cross-client', 'udp', [*short, '--clients', '2', '--rate', '100',
                                                   '--mode', 'proxy', '--fault', 'udp-cross-client'], True)
        result = json.loads(next(cross.glob('*/result.json')).read_text())
        assert 'client ownership mismatch' in result['primary_error']
        altered = result['backend_cleanup']['altered_echoes']
        assert len(altered) == 2 and all(r['sent_client'] != r['received_client'] for r in altered)
        disorder = execute('udp-disorder', 'udp', [*short, '--timeout', '.1', '--fault', 'udp-disorder'])
        for path in disorder.glob('*/result.json'):
            counts = json.loads(path.read_text())['counts']
            assert counts['duplicate'] > 0 and counts['late'] > 0 and counts['timeout'] > 0

    try:
        if args.suite == 'matrix':
            for protocol in ['tcp', 'udp']:
                execute(f'{protocol}-default', protocol)
                execute(f'{protocol}-clients16', protocol, ['--clients', '16', '--repeats', '1'])
        elif args.suite == 'failure':
            for fault in ['corrupt', 'background', 'product-exit', 'evidence', 'cleanup', 'resource', 'resource-evidence']:
                execute(fault, extra=[*short, '--mode', 'proxy', '--fault', fault], fail=True)
            udp_checks()
            execute('sigterm', extra=['--repeats', '1', '--mode', 'proxy'], fail=True, interrupt=True)
            execute('tcp-partial', extra=[*short, '--fault', 'tcp-partial'])
        elif args.suite == 'udp-check':
            udp_checks()
        elif args.suite == 'isolation':
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(execute, f'parallel-{protocol}', protocol, short) for protocol in ['tcp', 'udp']]
                for future in futures:
                    future.result()
            spaced = args.output / 'program with spaces'
            spaced.symlink_to(args.program.resolve())
            original = args.program
            args.program = spaced
            # Preserve the symlink spelling to exercise argv with spaces.
            runner_program = spaced.parent / 'binary with spaces'
            import shutil
            shutil.copy2(original, runner_program)
            args.program = runner_program
            execute('output with spaces', extra=short)
        else:
            assert os.getuid() != 0, 'permission negative must run as ordinary uid'
            output = execute('permission', extra=[*short, '--mode', 'proxy', '--fault', 'permission'], fail=True)
            result = json.loads(next(output.glob('*/result.json')).read_text())
            assert 'PermissionError' in result['primary_error'], result['primary_error']
    finally:
        (args.output / 'invocations.json').write_text(json.dumps(records, indent=2))
    print(json.dumps({'suite': args.suite, 'passed': len(records), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
