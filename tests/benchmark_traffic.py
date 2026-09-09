"""Bounded closed-loop TCP and paced aggregate-rate UDP generators."""
import concurrent.futures
import select
import socket
import struct
import threading
import time

HEADER = struct.Struct('!16sIIQ')
STRIDE = 100
MAX_PENDING = 65536


def packet(nonce, phase, client, seq, size):
    head = HEADER.pack(nonce, phase, client, seq)
    return head + b'x' * (size - len(head))


def counters():
    return dict(attempted=0, sent=0, received=0, timeout=0, duplicate=0,
                late=0, foreign_corrupt=0, send_errors=0, missed_slots=0,
                warmup_excluded=0, pending_peak=0)


def tcp_exchange(sock, data, deadline):
    """The RTT caller starts before the first attempt; handle partial writes/reads."""
    remaining = memoryview(data)
    while remaining:
        sock.settimeout(max(.001, deadline - time.monotonic()))
        if time.monotonic() >= deadline:
            raise TimeoutError('TCP send deadline')
        count = sock.send(remaining)
        if not count:
            raise RuntimeError('TCP closed during send')
        remaining = remaining[count:]
    output = bytearray()
    while len(output) < len(data):
        sock.settimeout(max(.001, deadline - time.monotonic()))
        if time.monotonic() >= deadline:
            raise TimeoutError('TCP receive deadline')
        value = sock.recv(len(data) - len(output))
        if not value:
            raise RuntimeError('TCP closed during receive')
        output.extend(value)
    if output != data:
        raise RuntimeError('foreign/corrupt TCP echo')


def tcp_phase(sockets, nonce, phase, seconds, timeout, payload, check):
    ready = threading.Barrier(len(sockets) + 1)
    begin = threading.Event()
    start = end = None

    def client_loop(index, sock):
        values = counters()
        samples = []
        seq = 0
        ready.wait(timeout=5)
        begin.wait(timeout=5)
        while time.monotonic_ns() < end:
            check()
            data = packet(nonce, phase, index, seq, payload)
            before = time.monotonic_ns()
            values['attempted'] += 1
            tcp_exchange(sock, data, before / 1e9 + timeout)
            after = time.monotonic_ns()
            values['sent'] += 1
            values['received'] += 1
            if seq % STRIDE == 0:
                samples.append({'client': index, 'seq': seq, 'rtt_ns': after - before})
            seq += 1
        return values, samples

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(sockets)) as pool:
        futures = [pool.submit(client_loop, i, sock) for i, sock in enumerate(sockets)]
        ready.wait(timeout=5)
        start = time.monotonic_ns()
        end = start + int(seconds * 1e9)
        begin.set()
        results = [future.result() for future in futures]
        finish = time.monotonic_ns()
    merged = counters()
    samples = []
    for values, rows in results:
        for name, value in values.items():
            merged[name] += value
        samples.extend(rows)
    return merged, samples, start, end, max(end, finish)


class DatagramLedger:
    """At most MAX_PENDING outstanding packets; compact bounded sequence history."""
    def __init__(self, nonce, phase, payload, slots, timeout):
        self.nonce, self.phase, self.payload = nonce, phase, payload
        self.timeout_ns = int(timeout * 1e9)
        self.states = bytearray(slots)
        # Retain ownership after pending removal, so duplicate/late echoes are
        # checked against the original sender too. Clients fit in one byte.
        self.owners = bytearray([255]) * slots
        self.pending = {}
        self.values = counters()
        self.samples = []

    def sent(self, seq, client, before):
        self.states[seq] = 1
        self.owners[seq] = client
        self.pending[seq] = (client, before)
        self.values['sent'] += 1
        self.values['pending_peak'] = max(self.values['pending_peak'], len(self.pending))

    def expire(self, now):
        # Insertion order is send-time order; only scan the expired prefix.
        while self.pending:
            seq = next(iter(self.pending))
            if now - self.pending[seq][1] < self.timeout_ns:
                break
            del self.pending[seq]
            self.states[seq] = 3
            self.values['timeout'] += 1

    def receive(self, data, client, now):
        if len(data) < HEADER.size:
            raise RuntimeError('foreign/corrupt UDP echo: short header')
        nonce, phase, found_client, seq = HEADER.unpack_from(data)
        if nonce == self.nonce and phase < self.phase:
            self.values['warmup_excluded'] += 1
            return
        if (nonce != self.nonce or phase != self.phase or found_client != client
                or seq >= len(self.states)
                or data != packet(nonce, phase, client, seq, self.payload)
                or self.states[seq] == 0):
            self.values['foreign_corrupt'] += 1
            raise RuntimeError('foreign/corrupt UDP echo')
        if self.owners[seq] != client:
            self.values['foreign_corrupt'] += 1
            raise RuntimeError('foreign/corrupt UDP echo: client ownership mismatch')
        self.expire(now)
        if self.states[seq] == 2:
            self.values['duplicate'] += 1
        elif self.states[seq] == 3:
            self.values['late'] += 1
        else:
            _, before = self.pending.pop(seq)
            self.states[seq] = 2
            self.values['received'] += 1
            if seq % STRIDE == 0:
                self.samples.append({'client': client, 'seq': seq, 'rtt_ns': now - before})


def udp_phase(sockets, nonce, phase, seconds, timeout, payload, rate, check):
    slots = int(seconds * rate)
    ledger = DatagramLedger(nonce, phase, payload, slots, timeout)
    start = time.monotonic_ns()
    end = start + int(seconds * 1e9)
    next_seq = 0
    indexed = {sock: i for i, sock in enumerate(sockets)}
    while True:
        check()
        now = time.monotonic_ns()
        ledger.expire(now)
        if now >= end and not ledger.pending:
            break
        if now < end and next_seq < slots:
            due = int((now - start) * rate // 1_000_000_000)
            if due > next_seq:
                ledger.values['missed_slots'] += min(due, slots) - next_seq
                next_seq = min(due, slots)
            if next_seq < slots and now >= start + next_seq * 1_000_000_000 // rate:
                if len(ledger.pending) >= MAX_PENDING:
                    ledger.values['missed_slots'] += 1
                else:
                    index = next_seq % len(sockets)
                    data = packet(nonce, phase, index, next_seq, payload)
                    before = time.monotonic_ns()
                    ledger.values['attempted'] += 1
                    try:
                        if sockets[index].send(data) != len(data):
                            raise OSError('short datagram send')
                        ledger.sent(next_seq, index, before)
                    except OSError:
                        ledger.values['send_errors'] += 1
                next_seq += 1
        next_time = start + next_seq * 1_000_000_000 // rate if next_seq < slots else end
        wait = min(.01, max(0, (next_time - time.monotonic_ns()) / 1e9)) if now < end else .001
        ready, _, _ = select.select(sockets, [], [], wait)
        for sock in ready:
            # Bound each receive batch so a busy socket cannot starve pacing/deadlines.
            for _ in range(64):
                try:
                    data = sock.recv(65536)
                except BlockingIOError:
                    break
                ledger.receive(data, indexed[sock], time.monotonic_ns())
    ledger.values['missed_slots'] += slots - next_seq
    ledger.values['target'] = slots
    return ledger.values, ledger.samples, start, end, max(end, time.monotonic_ns())
