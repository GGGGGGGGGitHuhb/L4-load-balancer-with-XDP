#!/usr/bin/env python3
"""Real CLI stop tests: observed pending barrier, absolute deadline, metrics and owned cleanup."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import subprocess
import struct
import threading
import time
import uuid


def require(value, why):
    if not value:
        raise RuntimeError(why)


def until(predicate, seconds=6):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.005)
    raise RuntimeError('state barrier timeout')


class Backend:
    def __init__(self, fault):
        self.fault = fault
        self.initial_fd = len(os.listdir('/proc/self/fd'))
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(64)
        self.listener.settimeout(.05)
        self.port = self.listener.getsockname()[1]
        self.done = threading.Event()
        self.ready = threading.Event()
        self.business = None
        self.clients = []
        self.workers = []
        self.probes = []
        self.errors = []
        self.worker = threading.Thread(target=self.accept)
        self.worker.start()

    def identify(self, client):
        try:
            client.settimeout(3)
            data = client.recv(8)
            if not data:
                self.probes.append(time.monotonic_ns())
                client.close()
                return
            require(data == b'identify', 'business marker')
            client.sendall(data)
            client.setblocking(False)
            self.business = client
            self.ready.set()
            if self.fault == "background":
                raise RuntimeError("injected backend thread exception")
        except BaseException as error:
            if not self.done.is_set():
                self.errors.append(str(error))

    def accept(self):
        try:
            while not self.done.is_set():
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.clients.append(client)
                worker = threading.Thread(target=self.identify, args=(client,))
                self.workers.append(worker)
                worker.start()
        except BaseException as error:
            if not self.done.is_set():
                self.errors.append(str(error))

    def close(self):
        self.done.set()
        self.worker.join(1)
        self.listener.close()
        for client in self.clients:
            client.close()
        for worker in self.workers:
            worker.join(4)
            require(not worker.is_alive(), 'backend worker joined')
        require(not self.worker.is_alive(), 'backend accept joined')
        require(not self.errors, 'backend exception: ' + ';'.join(self.errors))
        return {'fd_before': self.initial_fd, 'fd_after': len(os.listdir('/proc/self/fd')), 'probes_ns': self.probes}


def scenario(program, root, name, fault):
    path = root / name
    path.mkdir()
    result = {'scene': name, 'valid': False, 'primary_error': None, 'cleanup_errors': []}
    backend = Backend(fault)
    process = None
    client = None
    handles = []
    expected_failure = fault != 'none'
    start_fd = backend.initial_fd
    try:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        metrics = name != 'off'
        config = path / 'config.conf'
        config.write_text(f'listen=127.0.0.1:{port}\nbackend=127.0.0.1:{backend.port}\nhealth_check={"tcp_connect" if metrics else "off"}\nmetrics={"stderr" if metrics else "off"}\n')
        stdout = (path / 'stdout.log').open('w')
        stderr = (path / 'stderr.log').open('w')
        handles.extend([stdout, stderr])
        process = subprocess.Popen([str(program), '--run', str(config)], stdout=stdout, stderr=stderr)
        result['pid'] = process.pid

        def logs():
            return (path / 'stderr.log').read_text()

        def snapshots():
            return [json.loads(line[8:]) for line in logs().splitlines(keepends=True)
                    if line.startswith('metrics ') and line.endswith('\n')]

        until(lambda: '服务已启动' in (path / 'stdout.log').read_text())
        if metrics:
            until(lambda: any(s['backends'][0]['health'] == 'Healthy' for s in snapshots()))
        client = socket.socket()
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.settimeout(3)
        client.connect(('127.0.0.1', port))
        client.sendall(b'identify')
        require(client.recv(8) == b'identify', 'warm handshake')
        require(backend.ready.wait(3), 'backend business ready')
        client.setblocking(False)
        if name in ('deadline', 'forced'):
            pattern = [bytes(range(256)) * 256, bytes(reversed(range(256))) * 256]
            sources = [client, backend.business]
            submitted = [0, 0]
            blocked = [False, False]
            deadline = time.monotonic() + 8
            while not all(blocked):
                require(time.monotonic() < deadline, 'both producer sockets backpressured')
                for side, source in enumerate(sources):
                    try:
                        n = source.send(pattern[side])
                        submitted[side] += n
                    except BlockingIOError:
                        blocked[side] = True
            # A metrics plateau is a readiness condition, not a sleep guess.
            def plateau():
                s = [x for x in snapshots() if x['phase'] == 'periodic']
                return len(s) >= 2 and s[-1]['bytes_c2b_total'] > 8 and s[-1]['bytes_b2c_total'] > 8 and all(s[-1][key] == s[-2][key] for key in ('bytes_c2b_total', 'bytes_b2c_total'))
            until(plateau, 8)
            result['source_submitted'] = submitted
        if name == 'io-error':
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            client.close()
            client = None
            until(lambda: any(s['sessions_active'] == 0 and s['errors_total'] == 1 for s in snapshots()))
        sent_signal = signal.SIGINT if name in ('empty', 'off') else signal.SIGTERM
        before = time.monotonic_ns()
        process.send_signal(sent_signal)
        barrier = r'TCP draining pending_c2b=(\d+) pending_b2c=(\d+) deadline_ns=(\d+)\n'
        until(lambda: re.search(barrier, logs()) is not None, 2)
        barrier_seen = time.monotonic_ns()
        found = re.search(barrier, logs())
        require(found is not None, 'structured pending barrier')
        pending = [int(found[1]), int(found[2])]
        result.update(signal=int(sent_signal), signal_ns=before, barrier_observed_ns=barrier_seen, pending=pending, deadline_ns=int(found[3]))
        if name in ('deadline', 'forced'):
            require(all(pending), 'both user pending queues nonempty at consumed signal')
            with socket.socket() as rejected:
                rejected.settimeout(.2)
                require(rejected.connect_ex(('127.0.0.1', port)) != 0, 'listener removed during drain')
        if name == 'forced':
            process.send_signal(signal.SIGINT)
            result['second_signal_ns'] = time.monotonic_ns()
        code = process.wait(timeout=2)
        finish = time.monotonic_ns()
        result.update(returncode=code, elapsed_seconds=(finish - before) / 1e9, end_ns=finish)
        require(code == 0, 'normal signal exits 0')
        if name == 'deadline':
            require(.85 < result['elapsed_seconds'] < 2, 'production 1s absolute deadline')
            require('reason=service-stop-deadline' in logs(), 'deadline does not claim drained')
        elif name == 'forced':
            require((finish - result['second_signal_ns']) / 1e9 < .5, 'second observed signal forces promptly')
            require('reason=service-stop-forced' in logs(), 'forced reason')
        else:
            require(pending == [0, 0] and result['elapsed_seconds'] < .5, 'empty queues stop without EOF')
            if name == 'io-error':
                require('reason=SO_ERROR' in logs() or 'reason=recv' in logs(), 'real I/O failure close reason')
            else:
                require('reason=service-stop-drained' in logs(), 'empty drained reason')
        require('已尝试有界排空用户态待发队列，不保证在途数据送达' in logs(), 'bounded stop text')
        if metrics:
            parsed = snapshots()
            final = parsed[-1]
            require(final['phase'] == 'final' and final['sessions_active'] == 0, 'final after cleanup')
            require(final['sessions_created_total'] == final['sessions_closed_total'] == 1, 'Created/Closed once')
            require(final['timeouts_total'] == 0 and final['errors_total'] == (1 if name == 'io-error' else 0), 'stop never counts idle timeout/error')
            require(logs().rfind('reason=service-stop-') < logs().rfind('metrics '), 'close before final')
            closes = re.findall(r'sent_c2b=(\d+) sent_b2c=(\d+)', logs())
            require(len(closes) == 1 and list(map(int, closes[0])) == [final['bytes_c2b_total'], final['bytes_b2c_total']], 'successful send bytes counted once')
            require(not any(t > barrier_seen for t in backend.probes), 'no new health probes during drain')
            result['final_metrics'] = final
        else:
            require('metrics ' not in logs(), 'metrics off remains off')
        if fault == 'expectation':
            require(pending == [-1, -1], 'injected expected queue corruption')
        result['valid'] = True
    except BaseException as error:
        result['primary_error'] = str(error)
    finally:
        if process:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            result['reaped'] = process.poll() is not None and not Path(f'/proc/{process.pid}').exists()
        if client:
            client.close()
        for handle in handles:
            handle.close()
        try:
            result['backend'] = backend.close()
            if fault == 'cleanup':
                raise RuntimeError('injected cleanup audit failure after reap')
        except BaseException as error:
            result['cleanup_errors'].append(str(error))
        result['fixture_fd'] = {'before': start_fd, 'after': len(os.listdir('/proc/self/fd'))}
        if result['fixture_fd']['before'] != result['fixture_fd']['after']:
            result['cleanup_errors'].append('fixture fd leak')
        if result['primary_error'] or result['cleanup_errors']:
            result['valid'] = False
        (path / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    require(result['valid'] != expected_failure, json.dumps(result))
    require(result.get('reaped'), 'owned PID missing cleanup evidence')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--program', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--fault', choices=['none', 'expectation', 'background', 'cleanup'], default='none')
    args = parser.parse_args()
    root = args.output / uuid.uuid4().hex
    root.mkdir(parents=True)
    results = []
    for name in (['empty', 'deadline', 'forced', 'off', 'io-error'] if args.fault == 'none' else ['empty']):
        results.append(scenario(args.program.resolve(), root, name, args.fault))
    (root / 'summary.json').write_text(json.dumps({'argv': __import__('sys').argv, 'expected_fault': args.fault, 'results': results}, indent=2) + '\n')
    print(json.dumps({'output': str(root), 'passed': len(results), 'expected_fault': args.fault}))
    return 0 if all(r['valid'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
