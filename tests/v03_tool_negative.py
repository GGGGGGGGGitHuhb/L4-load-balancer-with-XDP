#!/usr/bin/env python3
"""Run four tool-failure classes and verify nonzero propagation plus cleanup."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    program = str(Path(sys.argv[1]).resolve())
    parent = Path(sys.argv[2]).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='negative-', dir=parent))
    script = Path(__file__).with_name('v03_system_test.py')
    cases = [
        ('wrong-backend', 'backend identity mismatch'),
        ('child-exit', 'product exited early code=17'),
        ('snapshot-seq', 'seq consecutive'),
        ('thread-error', 'backend thread failure: backend-thread-injected'),
        ('wrong-backend-cleanup', 'backend identity mismatch'),
    ]
    outcomes = []
    for fault, expected in cases:
        evidence = root / fault
        run = subprocess.run([sys.executable, str(script), program, str(evidence), 'tcp', '--fault', fault], capture_output=True, text=True, timeout=25)
        (root / (fault + '.stdout')).write_text(run.stdout)
        (root / (fault + '.stderr')).write_text(run.stderr)
        paths = [line[9:] for line in run.stdout.splitlines() if line.startswith('evidence=')]
        if run.returncode != 1 or len(paths) != 1:
            raise RuntimeError(f'{fault}: not target exit 1: {run.returncode}')
        path = Path(paths[0])
        result = json.loads((path / 'result.json').read_text())
        cleanup = json.loads((path / 'cleanup.json').read_text())
        if expected not in (result['primary'] or ''):
            raise RuntimeError(f'{fault}: original cause lost: {result}')
        if not cleanup['pid_absent'] or cleanup['before_fds'] != cleanup['after_fds']:
            raise RuntimeError(f'{fault}: resource cleanup failed')
        if fault == 'wrong-backend-cleanup':
            if result['cleanup_errors'] != ['cleanup-injected-after-resource-release']:
                raise RuntimeError('cleanup secondary failure not preserved')
        elif result['cleanup_errors']:
            raise RuntimeError(f'{fault}: unexpected cleanup failure')
        outcomes.append({'fault': fault, 'exit': run.returncode, 'primary': result['primary'], 'cleanup': cleanup, 'evidence': str(path)})
    (root / 'outcomes.json').write_text(json.dumps(outcomes, indent=2))
    print(f'PASS T1..T4 and primary-cause preservation: {root}', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print('FAIL tool validation: ' + str(error), file=sys.stderr)
        sys.exit(1)
