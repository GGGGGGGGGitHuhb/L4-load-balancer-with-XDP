"""Bounded open-loop UDP driver, echo fixture and raw resource capture."""
import json
import math
import os
from pathlib import Path
import select
import signal
import socket
import struct
import time

HEADER = struct.Struct('!8s16sBHI')
MAGIC = b'L4LBV12!'


def atomic_json(path, value):
    """Publish complete JSON before exists() can advertise it to a peer."""
    path = Path(path)
    pending = path.with_name(path.name + '.pending')
    # Each run owns this directory and publishes each handoff once. Keep a failed
    # pending file as evidence; never expose it under the reader's final name.
    with pending.open('x') as stream:
        json.dump(value, stream)
    os.replace(pending, path)


def packet(nonce, phase, client, seq, size):
    header = HEADER.pack(MAGIC, bytes.fromhex(nonce), phase, client, seq)
    return header + bytes([seq % 251]) * (size - len(header) - 1) + b'\0'


def echo(directory, index, fault):
    directory = Path(directory)
    sockets = []
    for address, port in [('198.19.0.100', 39001), (f'198.20.{index}.2', 39001), (f'198.20.{index}.2', 39009)]:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((address, port))
        s.setblocking(False)
        sockets.append(s)
    counts = dict(measurement_received=0, measurement_replied=0, probe_received=0, total_received=0)
    running = True
    def stop(_sig, _frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    (directory / f'echo-{index}.ready').write_text(str(os.getpid()))
    while running:
        for s in select.select(sockets, [], [], .02)[0]:
            for _ in range(256):
                try:
                    data, peer = s.recvfrom(4096)
                except BlockingIOError:
                    break
                counts['total_received'] += 1
                measurement = len(data) >= HEADER.size and data.startswith(MAGIC) and HEADER.unpack_from(data)[2] == 2
                if measurement:
                    counts['measurement_received'] += 1
                if s == sockets[-1]:
                    counts['probe_received'] += 1
                    response = data
                else:
                    response = data[:-1] + bytes([index + 1])
                    if fault == 'corrupt' and measurement:
                        response = response[:5] + b'?' + response[6:]
                    if fault == 'cross-client' and measurement:
                        fields = list(HEADER.unpack_from(response))
                        fields[3] = (fields[3] + 1) % 16
                        response = HEADER.pack(*fields) + response[HEADER.size:]
                try:
                    s.sendto(response, peer)
                    if measurement:
                        counts['measurement_replied'] += 1
                except BlockingIOError:
                    pass
    (directory / f'echo-{index}.json').write_text(json.dumps(counts))


def pid_sample(pid):
    ns = time.monotonic_ns()
    stat = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    if stat[0] == 'Z':
        raise RuntimeError(f'sampling exited PID {pid}')
    return dict(pid=pid, ns=ns, starttime=int(stat[19]), utime=int(stat[11]), stime=int(stat[12]),
                rss_bytes=int(stat[21]) * os.sysconf('SC_PAGE_SIZE'), fds=len(list(Path(f'/proc/{pid}/fd').iterdir())))


def system_sample():
    ns = time.monotonic_ns()
    stat = {row.split()[0]: list(map(int, row.split()[1:])) for row in Path('/proc/stat').read_text().splitlines() if row.startswith('cpu')}
    soft = {row.split(':')[0].strip(): list(map(int, row.split(':')[1].split())) for row in Path('/proc/softirqs').read_text().splitlines() if 'NET_RX:' in row or 'NET_TX:' in row}
    return dict(ns=ns, stat=stat, softirqs=soft)


def traffic(sockets, nonce, phase, size, rate, seconds, start, marker=None):
    end = start + int(seconds * 1e9)
    target = int(rate * seconds)
    counts = {key: 0 for key in ['target', 'attempted', 'sent', 'send_errors', 'missed_slots', 'unique_received', 'timeout', 'late', 'duplicate', 'corrupt', 'wrong_source']}
    counts['target'] = target
    states = bytearray(target)
    sent_ns = [0] * target
    pending = {}
    rtt, distribution = [], [0, 0]
    buckets = [dict(sent=0, received=0) for _ in range(math.ceil(seconds))]
    seq = 0
    def expire(now):
        while pending:
            key = next(iter(pending))
            if now - pending[key] < 1_000_000_000:
                break
            pending.pop(key)
            states[key] = 3
            counts['timeout'] += 1
    while time.monotonic_ns() < start:
        time.sleep(.0001)
    while True:
        now = time.monotonic_ns()
        expire(now)
        if now >= end and not pending:
            break
        if now < end and seq < target:
            due = min(target, (now - start) * rate // 1_000_000_000)
            if due > seq:
                counts['missed_slots'] += due - seq
                seq = due
            # A reply can wake select before the next open-loop slot. Never
            # let response arrival advance the offered workload.
            if seq < target and now >= start + seq * 1_000_000_000 // rate:
                client = seq % 16
                before = time.monotonic_ns()
                if before >= end or len(pending) >= 65536:
                    counts['missed_slots'] += 1
                    seq += 1
                    continue
                counts['attempted'] += 1
                try:
                    data = packet(nonce, phase, client, seq, size)
                    sockets[client].sendto(data, sockets[client].destination)
                    states[seq] = 1
                    sent_ns[seq] = before
                    pending[seq] = before
                    counts['sent'] += 1
                    if marker and counts['sent'] == 1:
                        marker.write_text(str(before))
                    buckets[min(len(buckets) - 1, (before - start) // 1_000_000_000)]['sent'] += 1
                except (OSError, BlockingIOError):
                    counts['send_errors'] += 1
                seq += 1
        next_ns = start + seq * 1_000_000_000 // rate
        wait = min(.005, max(0, (next_ns - time.monotonic_ns()) / 1e9)) if now < end else .001
        for sock in select.select(sockets, [], [], wait)[0]:
            client = sockets.index(sock)
            for _ in range(64):
                try:
                    data, peer = sock.recvfrom(4096)
                except BlockingIOError:
                    break
                received = time.monotonic_ns()
                if peer != sock.destination:
                    counts['wrong_source'] += 1
                    raise RuntimeError('wrong VIP/reply source')
                if len(data) < HEADER.size:
                    raise RuntimeError('corrupt reply short header')
                magic, found_nonce, found_phase, found_client, found_seq = HEADER.unpack_from(data)
                if magic == MAGIC and found_nonce.hex() == nonce and found_phase < phase:
                    continue
                if (magic != MAGIC or found_nonce.hex() != nonce or found_phase != phase or found_client != client or
                        found_seq >= target or found_seq % 16 != client or states[found_seq] == 0 or
                        data[-1] not in (1, 2) or data[:-1] != packet(nonce, phase, client, found_seq, size)[:-1]):
                    counts['corrupt'] += 1
                    raise RuntimeError('corrupt/cross-client reply')
                expire(received)
                if states[found_seq] == 2:
                    counts['duplicate'] += 1
                elif states[found_seq] == 3:
                    counts['late'] += 1
                else:
                    pending.pop(found_seq)
                    states[found_seq] = 2
                    counts['unique_received'] += 1
                    distribution[data[-1] - 1] += 1
                    before = sent_ns[found_seq]
                    buckets[min(len(buckets) - 1, (before - start) // 1_000_000_000)]['received'] += 1
                    if found_seq % 100 == 0:
                        rtt.append(dict(client=client, seq=found_seq, send_ns=before, receive_ns=received, rtt_ns=received - before))
    counts['missed_slots'] += target - seq
    return dict(counts=counts, rtt=rtt, backend_distribution=distribution, seconds=buckets,
                start_ns=start, end_ns=end, finish_ns=max(end, time.monotonic_ns()))


class ClientSocket(socket.socket):
    pass


def client(directory, spec, nonce):
    directory = Path(directory)
    sockets = []
    reply_examples = []
    destination = ('198.18.1.2', 39001) if spec['path'] == 'direct' else ('198.19.0.100', 39001)
    # Direct uses the same business listener/VIP through a direct L2 route.
    if spec['path'] == 'direct':
        destination = ('198.19.0.100', 39001)
    for i in range(16):
        s = ClientSocket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('198.18.0.1', 40000 + i))
        s.destination = destination
        s.settimeout(1)
        data = packet(nonce, 0, i, i, spec['payload'])
        s.sendto(data, destination)
        reply, peer = s.recvfrom(4096)
        if peer != destination or reply[:-1] != data[:-1] or reply[-1] not in (1, 2):
            raise RuntimeError('preflight source/payload')
        reply_examples.append(dict(client=i, source=list(peer), backend=reply[-1] - 1, header_hex=reply[:HEADER.size].hex()))
        s.setblocking(False)
        sockets.append(s)
    (directory / 'preflight.json').write_text(json.dumps(dict(reply_source_verified=True, received=16, reply_examples=reply_examples)))
    traffic(sockets, nonce, 1, spec['payload'], spec['rate'], spec['warmup_s'], time.monotonic_ns() + 10_000_000)
    (directory / 'client.ready').write_text(str(os.getpid()))
    while not (directory / 'go.json').exists():
        time.sleep(.002)
    start = json.loads((directory / 'go.json').read_text())
    result = traffic(sockets, nonce, 2, spec['payload'], spec['rate'], spec['window_s'], start, directory / 'traffic.started')
    atomic_json(directory / 'traffic.json', result)
    # Keep PID alive until both resource end barrier and drain have been sampled.
    while not (directory / 'client.stop').exists():
        time.sleep(.005)
