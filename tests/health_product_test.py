#!/usr/bin/env python3
"""真实产品默认时序；健康状态日志仅作屏障，业务使用独立 nonce。"""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time

PROGRAM = str(Path(sys.argv[1]).resolve())
ROOT = Path(sys.argv[2]).resolve()
ROOT.mkdir(parents=True, exist_ok=True)
MODE = sys.argv[3] if len(sys.argv) > 3 else "all"


def check(value, message):
    if not value:
        raise RuntimeError(message)


def until(predicate, message, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise RuntimeError(message)


def bind_socket(kind, port=0):
    sock = socket.socket(socket.AF_INET, kind)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    sock.settimeout(.1)
    return sock


class Backend:
    def __init__(self, udp=False):
        self.udp = bind_socket(socket.SOCK_DGRAM) if udp else None
        self.port = self.udp.getsockname()[1] if udp else 0
        self.listener = None
        self.done = threading.Event()
        self.threads = []
        self.clients = []
        self.payloads = []
        self.probes = 0
        self.errors = []
        if udp:
            self.launch(self.datagrams)
        else:
            self.start_tcp()

    def launch(self, target, *args):
        def guarded():
            try:
                target(*args)
            except Exception as error:
                if not self.done.is_set():
                    self.errors.append(repr(error))
        thread = threading.Thread(target=guarded)
        self.threads.append(thread)
        thread.start()

    def start_tcp(self):
        self.listener = bind_socket(socket.SOCK_STREAM, self.port)
        self.port = self.listener.getsockname()[1]
        self.listener.listen(64)
        self.launch(self.accept, self.listener)

    def stop_tcp(self):
        old = self.listener
        self.listener = None
        if old:
            old.close()
        # accept loop has a 100ms timeout and exits without closing clients.
        time.sleep(.12)

    def accept(self, listener):
        while not self.done.is_set() and self.listener is listener:
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            client.settimeout(.1)
            self.clients.append(client)
            self.launch(self.stream, client)

    def stream(self, client):
        received = False
        try:
            while not self.done.is_set():
                try:
                    data = client.recv(65536)
                except socket.timeout:
                    continue
                except ConnectionResetError:
                    break
                if not data:
                    break
                received = True
                self.payloads.append(data)
                client.sendall(data)
        finally:
            if not received:
                self.probes += 1
            client.close()

    def datagrams(self):
        while not self.done.is_set():
            try:
                data, source = self.udp.recvfrom(65536)
            except socket.timeout:
                continue
            self.payloads.append((source, data))
            self.udp.sendto(data, source)

    def close(self):
        self.done.set()
        if self.listener:
            self.listener.close()
        # Threads use finite socket timeouts; no external service is left running.
        for thread in self.threads:
            thread.join(2)
            check(not thread.is_alive(), "backend thread leaked")
        if self.udp:
            self.udp.close()
        check(not self.errors, "backend errors: " + str(self.errors))


class Product:
    def __init__(self, name, backend, protocol, health):
        self.path = ROOT / name
        self.path.mkdir(exist_ok=True)
        reservation = bind_socket(socket.SOCK_STREAM if protocol == "tcp" else socket.SOCK_DGRAM)
        self.port = reservation.getsockname()[1]
        reservation.close()
        config = self.path / "config.conf"
        config.write_text(f"listen=127.0.0.1:{self.port}\nbackend=127.0.0.1:{backend.port}\nprotocol={protocol}\nhealth_check={health}\n")
        self.stdout = open(self.path / "stdout.log", "w")
        self.stderr = open(self.path / "stderr.log", "w")
        self.process = subprocess.Popen([PROGRAM, "--run", str(config)], stdout=self.stdout, stderr=self.stderr)
        self.pid = self.process.pid
        self.started = time.monotonic()
        try:
            until(lambda: "服务已启动" in (self.path / "stdout.log").read_text(), "product ready missing", 3)
            self.initial_fds = len(list(Path(f"/proc/{self.pid}/fd").iterdir()))
        except BaseException:
            self.close()
            raise

    def logs(self):
        return (self.path / "stderr.log").read_text()

    def state(self, transition, count=1):
        until(lambda: self.logs().count(transition) >= count, "missing health transition " + transition)
        check(self.process.poll() is None, "product exited during health transition")

    def stream(self):
        client = socket.create_connection(("127.0.0.1", self.port), timeout=1)
        client.settimeout(.5)
        return client

    def reject_tcp(self, nonce):
        with self.stream() as client:
            try:
                client.sendall(nonce)
                data = client.recv(1024)
                check(not data, "all-bad TCP unexpectedly forwarded")
            except (ConnectionResetError, BrokenPipeError):
                pass
            except socket.timeout:
                raise RuntimeError("all-bad TCP client retained")

    def close(self):
        if self.process.poll() is None:
            start = time.monotonic()
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise RuntimeError("stop starved beyond 2 seconds")
            elapsed = time.monotonic() - start
            check(elapsed < 2, "stop bound")
        self.stdout.close()
        self.stderr.close()
        check(self.process.returncode == 0, "product failed exit=" + str(self.process.returncode))
        check(not Path(f"/proc/{self.pid}").exists(), "product PID survives")
        (self.path / "cleanup.txt").write_text(f"pid={self.pid} exit=0 proc_absent=true initial_fd_count={self.initial_fds}\n")


def tcp_echo(client, payload):
    client.sendall(payload)
    received = b""
    while len(received) < len(payload):
        part = client.recv(len(payload) - len(received))
        check(part, "existing TCP closed by health")
        received += part
    check(received == payload, "TCP payload differs")


def udp_echo(client, product, payload, backend):
    client.sendto(payload, ("127.0.0.1", product.port))
    data, source = client.recvfrom(65536)
    check(data == payload and source == ("127.0.0.1", product.port), "UDP data/source differs")
    matches = [item for item in backend.payloads if item[1] == payload]
    check(len(matches) == 1, "UDP backend nonce missing/duplicate")
    return matches[0][0]


def udp_drop(client, product, payload, backend):
    client.sendto(payload, ("127.0.0.1", product.port))
    try:
        client.recvfrom(65536)
        raise RuntimeError("all-bad UDP unexpectedly forwarded")
    except socket.timeout:
        pass
    check(not any(item[1] == payload for item in backend.payloads), "unavailable new UDP flow reached backend")


def tcp_scenario():
    backend = Backend()
    product = None
    hot_stop = threading.Event()
    hot_errors = []
    hot = None
    old = None
    try:
        product = Product("tcp-health", backend, "tcp", "tcp_connect")
        product.reject_tcp(b"unknown-tcp")
        check(not backend.payloads, "Unknown TCP sent business payload")
        product.state("from=Unknown to=Healthy")
        check(time.monotonic() - product.started >= 1, "healthy before two default probes")
        until(lambda: backend.probes >= 2, "probes not classified empty")
        old = product.stream()
        tcp_echo(old, b"old-tcp-before")
        def traffic():
            try:
                counter = 0
                while not hot_stop.is_set():
                    tcp_echo(old, b"hot-tcp-" + str(counter).encode() + b"x" * 1024)
                    counter += 1
                    time.sleep(.001)
            except Exception as error:
                hot_errors.append(repr(error))
        hot = threading.Thread(target=traffic)
        hot.start()
        backend.stop_tcp()
        product.state("from=Healthy to=Unhealthy")
        product.reject_tcp(b"all-bad-tcp")
        check(b"all-bad-tcp" not in backend.payloads, "all-bad TCP fallback payload")
        check(not hot_errors, "existing hot TCP interrupted: " + str(hot_errors))
        backend.start_tcp()
        product.state("from=Unhealthy to=Healthy")
        with product.stream() as client:
            tcp_echo(client, b"recovered-tcp")
        check(not hot_errors, "existing TCP recovery interrupted")
        hot_stop.set(); hot.join(2)
        check(not hot.is_alive(), "TCP traffic thread stuck")
        tcp_echo(old, b"old-tcp-after")
        # SIGTERM while data is continuously ready must still meet the stop bound.
        flood_stop = threading.Event()
        def flood():
            while not flood_stop.is_set():
                try:
                    old.sendall(b"stop-load" * 128)
                    old.recv(8192)
                except OSError:
                    return
        flood_thread = threading.Thread(target=flood)
        flood_thread.start()
        try:
            product.close(); product = None
        finally:
            flood_stop.set(); flood_thread.join(2)
        check(not flood_thread.is_alive(), "TCP stop traffic leaked")
        print(f"PASS TCP default Unknown/2success/3failure/2success old-session/hot-maintenance/stop probes={backend.probes}", flush=True)
    finally:
        hot_stop.set()
        if hot:
            hot.join(2)
        if old:
            old.close()
        if product:
            product.close()
        backend.close()


def udp_scenario():
    backend = Backend(udp=True)
    product = None
    clients = []
    def client():
        value = bind_socket(socket.SOCK_DGRAM); value.settimeout(.3); clients.append(value); return value
    try:
        product = Product("udp-off", backend, "udp", "off")
        udp_echo(client(), product, b"off-only-udp", backend)
        time.sleep(1.2)
        check(backend.probes == 0 and "health backend=" not in product.logs(), "off probed")
        product.close(); product = None
        product = Product("udp-health", backend, "udp", "tcp_connect")
        check("TCP 健康端点" in product.logs(), "UDP startup convention missing")
        udp_drop(client(), product, b"unknown-udp", backend)
        product.state("from=Unknown to=Unhealthy")
        udp_drop(client(), product, b"udp-only-unhealthy", backend)
        backend.start_tcp()
        product.state("from=Unhealthy to=Healthy")
        old = client()
        binding = udp_echo(old, product, b"old-udp-before", backend)
        backend.stop_tcp()
        # Hot existing flow keeps receive ready while probes fail on independent TCP.
        deadline = time.monotonic() + 6
        counter = 0
        while "from=Healthy to=Unhealthy" not in product.logs() and time.monotonic() < deadline:
            check(udp_echo(old, product, b"hot-udp-" + str(counter).encode(), backend) == binding, "hot UDP flow changed binding")
            counter += 1
            time.sleep(.001)
        product.state("from=Healthy to=Unhealthy")
        check(udp_echo(old, product, b"old-udp-unhealthy", backend) == binding, "unhealthy old flow migrated")
        udp_drop(client(), product, b"new-udp-unhealthy", backend)
        backend.start_tcp()
        product.state("from=Unhealthy to=Healthy", count=2)
        udp_echo(client(), product, b"new-udp-recovered", backend)
        check(udp_echo(old, product, b"old-udp-recovered", backend) == binding, "recovered old flow migrated")
        (ROOT / "udp-data.txt").write_text(f"old_binding={binding} hot_nonce_count={counter} all_nonce_count={len(backend.payloads)} probes={backend.probes}\n")
        # Continuous unreceived traffic also must not starve shutdown.
        stop = threading.Event()
        def flood():
            while not stop.is_set():
                old.sendto(b"stop-load", ("127.0.0.1", product.port))
                time.sleep(.0001)
        thread = threading.Thread(target=flood)
        thread.start()
        try:
            product.close()
        finally:
            stop.set(); thread.join(2)
        product = None
        print(f"PASS UDP off-only/proxy-endpoint/default-threshold/old-flow-binding/new-key-drop/recovery hot_nonce_count={counter}", flush=True)
    finally:
        for item in clients:
            item.close()
        if product:
            product.close()
        backend.close()


def tcp_off():
    backend = Backend()
    product = None
    try:
        product = Product("tcp-off", backend, "tcp", "off")
        with product.stream() as client:
            tcp_echo(client, b"off-tcp")
        time.sleep(1.3)
        check(backend.probes == 0, "off added empty connections")
        check("health backend=" not in product.logs(), "off health output")
        print("PASS TCP off has zero probe connections", flush=True)
    finally:
        if product:
            product.close()
        backend.close()


try:
    if MODE in ("all", "tcp"):
        tcp_off()
        tcp_scenario()
    if MODE in ("all", "udp"):
        udp_scenario()
except Exception as error:
    print("FAIL health product:", error, file=sys.stderr, flush=True)
    sys.exit(1)
