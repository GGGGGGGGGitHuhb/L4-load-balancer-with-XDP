"""V1.0 static-check and startup/stop contract, using only owned loopback ports."""
import hashlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def check(value, message):
    if not value:
        raise AssertionError(message)


def run(program, config):
    return subprocess.run([program, '--check-config', str(config)],
                          capture_output=True, timeout=3)


def main():
    program = str(Path(sys.argv[1]).resolve())
    root = Path(sys.argv[2])
    root.mkdir(parents=True, exist_ok=True)
    pids = []
    ports = []
    initial_fds = len(os.listdir('/proc/self/fd'))
    with tempfile.TemporaryDirectory(prefix='contract-', dir=root) as temporary:
        config = Path(temporary) / 'static.conf'
        # The reserved port is deliberately occupied; the backend has no route in
        # the validation netns. Static checking must still succeed in all modes.
        for protocol, kind in [('tcp', socket.SOCK_STREAM), ('udp', socket.SOCK_DGRAM)]:
            with socket.socket(socket.AF_INET, kind) as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
                ports.append((kind, port))
                if protocol == 'tcp':
                    reservation.listen()
                for health in ['off', 'tcp_connect']:
                    for metrics in ['off', 'stderr']:
                        config.write_text(f'listen=127.0.0.1:{port}\nbackend=192.0.2.1:9\n'
                                          f'protocol={protocol}\nhealth_check={health}\nmetrics={metrics}\n')
                        before = hashlib.sha256(config.read_bytes()).digest()
                        result = run(program, config)
                        check(result.returncode == 0, 'static exit code')
                        check(result.stdout == f'配置有效：{protocol.upper()}，后端数量=1\n'.encode(), 'static stdout')
                        check(result.stderr == b'', 'static stderr')
                        check(hashlib.sha256(config.read_bytes()).digest() == before, 'config rewritten')
                failed = subprocess.run([program, '--run', str(config)], capture_output=True, timeout=3)
                check(failed.returncode == 1 and failed.stdout == b'', 'bind failure code/ready')
                check('服务错误：' in failed.stderr.decode(), 'missing service error category')
            # Reuse that exact listener address to verify failure left no owner.
            config.write_text(f'listen=127.0.0.1:{port}\nbackend=192.0.2.1:9\nprotocol={protocol}\n')
            with (root / f'{protocol}.stdout').open('wb') as out, (root / f'{protocol}.stderr').open('wb') as err:
                process = subprocess.Popen([program, '--run', str(config)], stdout=out, stderr=err)
                pids.append(process.pid)
                try:
                    deadline = time.monotonic() + 3
                    while not (root / f'{protocol}.stdout').read_bytes() and time.monotonic() < deadline:
                        check(process.poll() is None, 'exit before ready')
                        time.sleep(.01)
                    check((root / f'{protocol}.stdout').read_bytes() ==
                          f'{protocol.upper()} 服务已启动：127.0.0.1:{port}\n'.encode(), 'ready exact format')
                    process.send_signal(signal.SIGTERM)
                    check(process.wait(timeout=4) == 0, 'normal stop exit')
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=3)
                check(b'metrics ' not in (root / f'{protocol}.stderr').read_bytes(), 'off emitted metrics')
            with socket.socket(socket.AF_INET, kind) as reclaimed:
                reclaimed.bind(('127.0.0.1', port))
    check(all(not Path(f'/proc/{pid}').exists() for pid in pids), 'PID leak')
    check(len(os.listdir('/proc/self/fd')) == initial_fds, 'fd leak')
    check(not list(root.glob('contract-*')), 'temporary config leak')
    (root / 'cleanup.txt').write_text(f'pids_reaped={pids}\nports_rebound={ports}\nfd_before_after={initial_fds}\ntemporary_configs_removed=true\n')
    print('PASS static 8 combinations/hash, TCP/UDP bind-failure/ready/stop/off, PID/fd/port/temp cleanup')


if __name__ == '__main__':
    main()
