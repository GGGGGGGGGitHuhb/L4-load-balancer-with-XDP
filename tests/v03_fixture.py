"""S3 own-socket fixture, independent data ledger and failure-safe evidence."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from metrics_product_test import snapshots


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def bound(kind, port=0):
    value = socket.socket(socket.AF_INET, kind)
    try:
        value.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        value.bind(('127.0.0.1', port))
        value.settimeout(.05)
        return value
    except Exception:
        value.close()
        raise


class Backend:
    def __init__(self, suite, index, udp):
        self.suite = suite
        self.index = index
        self.done = threading.Event()
        self.threads = []
        self.clients = []
        self.events = []
        self.errors = []
        self.probes = 0
        self.listener = None
        self.udp = None
        self.port = 0
        try:
            if udp:
                self.udp = bound(socket.SOCK_DGRAM)
                self.port = self.udp.getsockname()[1]
                self.launch(self.datagrams)
            self.start_tcp()
        except Exception:
            self.close()
            raise

    def launch(self, target, *args, close=None):
        def guarded():
            try:
                target(*args)
            except Exception as error:
                if not self.done.is_set():
                    self.errors.append(str(error))
            finally:
                if close is not None:
                    close.close()
        worker = threading.Thread(target=guarded)
        self.threads.append(worker)
        worker.start()

    def start_tcp(self):
        self.listener = bound(socket.SOCK_STREAM, self.port)
        self.port = self.listener.getsockname()[1]
        self.listener.listen(64)
        self.launch(self.accept, self.listener)
        self.suite.action('health-listen', backend=self.index, port=self.port)

    def stop_tcp(self):
        value = self.listener
        self.listener = None
        if value is not None:
            value.close()
        self.suite.action('health-stop', backend=self.index)
        time.sleep(.06)

    def accept(self, listener):
        while not self.done.is_set() and self.listener is listener:
            try:
                client, source = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if self.listener is not listener or self.done.is_set():
                    return
                raise
            client.settimeout(.05)
            self.clients.append(client)
            self.launch(self.stream, client, source, close=client)

    def fault(self):
        if self.suite.fault == 'thread-error':
            raise RuntimeError('backend-thread-injected')

    def stream(self, client, source):
        pending = b''
        observed = False
        while not self.done.is_set():
            try:
                data = client.recv(65536)
            except socket.timeout:
                continue
            except ConnectionResetError:
                break
            if not data:
                break
            pending += data
            while len(pending) >= 4:
                length = struct.unpack('!I', pending[:4])[0]
                require(length <= 65536, 'fixture frame too large')
                if len(pending) < 4 + length:
                    break
                frame = pending[:4 + length]
                pending = pending[4 + length:]
                observed = True
                self.fault()
                self.events.append({'time': time.monotonic(), 'nonce': frame[4:].hex(), 'source': source, 'bytes': len(frame)})
                client.sendall(frame)
        if not observed:
            self.probes += 1

    def datagrams(self):
        while not self.done.is_set():
            try:
                data, source = self.udp.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                if self.done.is_set():
                    return
                raise
            self.fault()
            self.events.append({'time': time.monotonic(), 'nonce': data.hex(), 'source': source, 'bytes': len(data)})
            self.udp.sendto(data, source)

    def close(self):
        self.done.set()
        if self.listener is not None:
            self.listener.close()
        if self.udp is not None:
            self.udp.close()
        for client in self.clients:
            client.close()
        for worker in self.threads:
            worker.join(1)
            require(not worker.is_alive(), 'backend thread cleanup timeout')
        self.suite.action('backend-cleanup', backend=self.index, threads=len(self.threads), probes=self.probes)


class Product:
    def __init__(self, suite, protocol, endpoints, health):
        self.suite = suite
        self.process = None
        self.stdout = None
        self.stderr = None
        self.exit_code = None
        self.port = 0
        self.protocol = protocol
        self.endpoints = endpoints
        self.health = health

    def start(self):
        reserve = bound(socket.SOCK_STREAM if self.protocol == 'tcp' else socket.SOCK_DGRAM)
        self.port = reserve.getsockname()[1]
        reserve.close()
        config = self.suite.root / 'config.conf'
        config.write_text(f'listen=127.0.0.1:{self.port}\nprotocol={self.protocol}\nhealth_check={self.health}\nmetrics=stderr\n' + ''.join(f'backend=127.0.0.1:{port}\n' for port in self.endpoints))
        self.stdout = open(self.suite.root / 'stdout.log', 'w')
        self.stderr = open(self.suite.root / 'stderr.log', 'w')
        command = [self.suite.program, '--run', str(config)]
        if self.suite.fault == 'child-exit':
            command = [sys.executable, '-c', 'raise SystemExit(17)']
        self.process = subprocess.Popen(command, stdout=self.stdout, stderr=self.stderr)
        self.suite.action('product-start', pid=self.process.pid, argv=command)
        self.suite.wait(lambda: '服务已启动' in (self.suite.root / 'stdout.log').read_text(), 'missing ready', 3)

    def logs(self):
        value = (self.suite.root / 'stderr.log').read_text()
        if self.suite.fault == 'snapshot-seq':
            lines = value.splitlines(keepends=True)
            for index, line in enumerate(lines):
                if line.startswith('metrics ') and line.endswith('\n'):
                    decoded = json.loads(line[8:])
                    decoded['seq'] += 1
                    self.suite.write('corrupt-snapshot.json', decoded)
                    lines[index] = 'metrics ' + json.dumps(decoded) + '\n'
                    break
            return ''.join(lines)
        return value

    def parsed(self):
        values = snapshots(self)
        self.suite.write('snapshots.json', values)
        return values

    def stop(self):
        if self.process is None:
            return
        start = time.monotonic()
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise RuntimeError('product stop exceeded 2s; killed and reaped')
        self.exit_code = self.process.returncode
        self.suite.action('product-stop', pid=self.process.pid, exit=self.exit_code, elapsed=time.monotonic() - start)
        require(not Path(f'/proc/{self.process.pid}').exists(), 'owned product PID remains')

    def close_files(self):
        if self.stdout is not None:
            self.stdout.close()
        if self.stderr is not None:
            self.stderr.close()


class Suite:
    def __init__(self, program, evidence, scenario, fault=''):
        self.program = str(Path(program).resolve())
        Path(evidence).mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix='run-', dir=Path(evidence).resolve()))
        self.scenario = scenario
        self.fault = fault
        self.backends = []
        self.clients = []
        self.product = None
        self.actions = []
        self.ledger = []
        self.cursor = 0
        self.serial = 0
        self.counts = {key: 0 for key in ['sessions_created_total', 'sessions_closed_total', 'sessions_active', 'bytes_c2b_total', 'bytes_b2c_total', 'datagrams_c2b_total', 'datagrams_b2c_total', 'rejected_total', 'dropped_datagrams_total', 'errors_total', 'timeouts_total']}
        self.before_fds = len(list(Path('/proc/self/fd').iterdir()))
        print('evidence=' + str(self.root), flush=True)

    def write(self, name, value):
        (self.root / name).write_text(json.dumps(value, indent=2))

    def action(self, kind, **values):
        self.actions.append({'time': time.monotonic(), 'kind': kind, **values})

    def check_workers(self):
        for backend in self.backends:
            require(not backend.errors, 'backend thread failure: ' + '; '.join(backend.errors))
        if self.product is not None and self.product.process is not None:
            code = self.product.process.poll()
            require(code is None, f'product exited early code={code}')

    def wait(self, predicate, reason, timeout=7):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.check_workers()
            if predicate():
                return
            time.sleep(.01)
        raise RuntimeError(reason)

    def choose(self, eligible):
        require(any(eligible), 'model cannot choose empty pool')
        for _ in eligible:
            index = self.cursor
            self.cursor = (self.cursor + 1) % len(eligible)
            if eligible[index]:
                return index
        raise RuntimeError('model cursor exhausted')

    def nonce(self, label):
        self.serial += 1
        return f'{self.scenario}-{label}-{self.serial}'.encode() + b'\x00\xff'

    def health(self, expected):
        def ready():
            values = self.product.parsed()
            return bool(values) and [b['health'] for b in values[-1]['backends']] == expected
        self.wait(ready, 'health barrier missing ' + repr(expected))
        self.action('health-barrier', expected=expected)

    def add_created(self):
        self.counts['sessions_created_total'] += 1
        self.counts['sessions_active'] += 1

    def account(self, payload, backend, source=None):
        size = len(payload) + (4 if self.scenario != 'udp' else 0)
        self.counts['bytes_c2b_total'] += size
        self.counts['bytes_b2c_total'] += size
        if self.scenario == 'udp':
            self.counts['datagrams_c2b_total'] += 1
            self.counts['datagrams_b2c_total'] += 1
        self.ledger.append({'nonce': payload.hex(), 'backend': backend, 'source': source, 'submitted_each_direction': size})

    def metrics(self, label):
        actual = None
        def matched():
            nonlocal actual
            rows = self.product.parsed()
            if not rows:
                return False
            actual = rows[-1]
            return all(actual[key] == value for key, value in self.counts.items())
        try:
            self.wait(matched, 'metric ledger mismatch ' + label, 4)
        finally:
            self.action('ledger-check', label=label, expected=dict(self.counts), actual=actual)
            self.write('ledger.json', {'expected': self.counts, 'data': self.ledger, 'actions': self.actions})

    def finish(self):
        self.product.stop()
        require(self.product.exit_code == 0, 'nonzero normal product exit')
        self.counts['sessions_closed_total'] += self.counts['sessions_active']
        self.counts['sessions_active'] = 0
        rows = self.product.parsed()
        require(len([s for s in rows if s['phase'] in ['final', 'error']]) == 1 and rows[-1]['phase'] == 'final', 'single final missing')
        require(all(rows[-1][key] == value for key, value in self.counts.items()), 'final independent ledger mismatch')
        self.action('final-ledger', expected=dict(self.counts), actual=rows[-1])

    def cleanup(self):
        errors = []
        if self.product is not None:
            try:
                self.product.stop()
            except Exception as error:
                errors.append(str(error))
            try:
                self.product.close_files()
            except Exception as error:
                errors.append(str(error))
        for client in self.clients:
            try:
                client.close()
            except Exception as error:
                errors.append(str(error))
        for backend in self.backends:
            try:
                backend.close()
            except Exception as error:
                errors.append(str(error))
        after = len(list(Path('/proc/self/fd').iterdir()))
        if after != self.before_fds:
            errors.append(f'fixture fd mismatch before={self.before_fds} after={after}')
        if self.fault == 'wrong-backend-cleanup':
            errors.append('cleanup-injected-after-resource-release')
        self.write('backend-events.json', [{'index': b.index, 'events': b.events, 'probes': b.probes, 'errors': b.errors} for b in self.backends])
        self.write('actions.json', self.actions)
        self.write('ledger.json', {'expected': self.counts, 'data': self.ledger})
        self.write('cleanup.json', {'errors': errors, 'before_fds': self.before_fds, 'after_fds': after, 'pid': self.product.process.pid if self.product and self.product.process else None, 'exit': self.product.exit_code if self.product else None, 'pid_absent': not self.product or not self.product.process or not Path(f'/proc/{self.product.process.pid}').exists()})
        return errors
