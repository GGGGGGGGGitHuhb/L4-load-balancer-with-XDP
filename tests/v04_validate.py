#!/usr/bin/env python3
"""Opt-in lifecycle isolation and oracle/cleanup negatives, with preserved invocation evidence."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--state-test', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    calls = []
    script = Path(__file__).with_name('v04_shutdown_product.py')

    def execute(name, argv, expected=0, needle=None):
        completed = subprocess.run(argv, text=True, capture_output=True, timeout=90)
        (args.output / f'{name}.stdout.log').write_text(completed.stdout)
        (args.output / f'{name}.stderr.log').write_text(completed.stderr)
        record = {'name': name, 'argv': argv, 'returncode': completed.returncode, 'expected': expected}
        calls.append(record)
        (args.output / f'{name}.invocation.json').write_text(json.dumps(record, indent=2))
        assert completed.returncode == expected, record
        if needle:
            assert needle in completed.stderr, completed.stderr

    def product(name, program, fault='none'):
        execute(name, [sys.executable, str(script), '--program', str(program.resolve()), '--output', str(args.output / name), '--fault', fault], 0 if fault == 'none' else 1)
        results = list((args.output / name).glob('*/**/result.json'))
        assert results
        for path in results:
            result = json.loads(path.read_text())
            assert result['valid'] == (fault == 'none')
            assert result['reaped'] and not Path(f"/proc/{result['pid']}").exists()
            assert result['fixture_fd']['before'] == result['fixture_fd']['after']
            if fault != 'none':
                assert result['primary_error'] or result['cleanup_errors']

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(product, f'parallel-{i}', args.program) for i in range(2)]
            for future in futures:
                future.result()
        spaced = args.output / 'program with spaces'
        shutil.copy2(args.program, spaced)
        product('output with spaces', spaced)
        for fault in ['expectation', 'background', 'cleanup']:
            product(fault, args.program, fault)
        execute('wrong-bytes', [str(args.state_test.resolve()), 'bytes'], 1, 'exact pending bytes')
        execute('extended-deadline', [str(args.state_test.resolve()), 'deadline'], 1, 'bounded absolute deadline')
    finally:
        (args.output / 'invocations.json').write_text(json.dumps(calls, indent=2) + '\n')
    print(json.dumps({'passed': len(calls), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
