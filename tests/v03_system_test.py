#!/usr/bin/env python3
"""V0.3 finite fault matrix against the unmodified product CLI."""
import argparse
import socket
import struct
import sys
import time

from v03_fixture import Backend, Product, Suite, bound, require


def receive(suite, client, count):
    data = b''
    deadline = time.monotonic() + 2
    while len(data) < count and time.monotonic() < deadline:
        suite.check_workers()
        try:
            part = client.recv(count - len(data))
        except socket.timeout:
            continue
        suite.check_workers()
        require(part, 'unexpected business EOF')
        data += part
    require(len(data) == count, 'business receive deadline')
    return data


def tcp_client(suite):
    value = socket.create_connection(('127.0.0.1', suite.product.port), timeout=1)
    value.settimeout(.05)
    suite.clients.append(value)
    return value


def exchange(suite, client, expected, label, zero=False, binding=None):
    payload = b'' if zero else suite.nonce(label)
    before = [len(backend.events) for backend in suite.backends]
    if suite.scenario == 'udp':
        client.sendto(payload, ('127.0.0.1', suite.product.port))
        end = time.monotonic() + 2
        while True:
            suite.check_workers()
            try:
                data, source = client.recvfrom(65536)
                break
            except socket.timeout:
                require(time.monotonic() < end, 'UDP reply deadline')
        require(data == payload and source == ('127.0.0.1', suite.product.port), 'UDP nonce/reply source differs')
    else:
        frame = struct.pack('!I', len(payload)) + payload
        client.sendall(frame)
        require(receive(suite, client, len(frame)) == frame, 'TCP nonce differs')
    observed = []
    for backend, start in zip(suite.backends, before):
        for event in backend.events[start:]:
            if event['nonce'] == payload.hex():
                observed.append((backend.index, event['source']))
    require(len(observed) == 1, 'nonce missing or duplicated across backends')
    wanted = expected
    if suite.fault in ['wrong-backend', 'wrong-backend-cleanup']:
        wanted = 1 - expected
    require(observed[0][0] == wanted, f'backend identity mismatch expected={wanted} actual={observed[0][0]}')
    if binding is not None:
        require(observed[0][1] == binding, 'old UDP source binding migrated')
    suite.account(payload, expected, observed[0][1])
    suite.action('exchange', backend=expected, nonce=payload.hex(), source=observed[0][1])
    return observed[0][1]


def close_tcp(suite, client):
    client.shutdown(socket.SHUT_WR)
    deadline = time.monotonic() + 2
    while True:
        suite.check_workers()
        try:
            require(client.recv(1) == b'', 'unexpected trailing TCP bytes')
            break
        except socket.timeout:
            require(time.monotonic() < deadline, 'TCP close deadline')
    client.close()
    suite.counts['sessions_closed_total'] += 1
    suite.counts['sessions_active'] -= 1


def new_business(suite, eligible, label, keep=False):
    expected = suite.choose(eligible)
    if suite.scenario == 'udp':
        value = bound(socket.SOCK_DGRAM)
        suite.clients.append(value)
    else:
        value = tcp_client(suite)
    suite.add_created()
    source = exchange(suite, value, expected, label)
    if suite.scenario != 'udp' and not keep:
        close_tcp(suite, value)
    return value, expected, source


def rejected(suite, label, business_failure=False):
    payload = suite.nonce(label)
    if suite.scenario == 'udp':
        client = bound(socket.SOCK_DGRAM)
        suite.clients.append(client)
        client.sendto(payload, ('127.0.0.1', suite.product.port))
        deadline = time.monotonic() + .35
        while time.monotonic() < deadline:
            suite.check_workers()
            try:
                client.recvfrom(65536)
                raise RuntimeError('unavailable UDP key received reply')
            except socket.timeout:
                pass
        suite.counts['rejected_total'] += 1
        suite.counts['dropped_datagrams_total'] += 1
    else:
        client = tcp_client(suite)
        frame = struct.pack('!I', len(payload)) + payload
        deadline = time.monotonic() + 2
        try:
            client.sendall(frame)
            while True:
                suite.check_workers()
                try:
                    require(client.recv(65536) == b'', 'failed TCP client received fallback data')
                    break
                except socket.timeout:
                    require(time.monotonic() < deadline, 'failed TCP client retained')
        except (ConnectionResetError, BrokenPipeError):
            pass
        client.close()
        if business_failure:
            suite.counts['sessions_created_total'] += 1
            suite.counts['sessions_closed_total'] += 1
            suite.counts['errors_total'] += 1
        else:
            suite.counts['rejected_total'] += 1
    require(not any(event['nonce'] == payload.hex() for backend in suite.backends for event in backend.events), 'failed nonce reached backend')
    suite.action('rejected', nonce=payload.hex(), business_failure=business_failure)


def pool(suite):
    udp = suite.scenario == 'udp'
    for index in range(2):
        suite.backends.append(Backend(suite, index, udp))
    suite.product = Product(suite, 'udp' if udp else 'tcp', [b.port for b in suite.backends], 'tcp_connect')
    suite.product.start()
    suite.health(['Healthy', 'Healthy'])
    old = [new_business(suite, [True, True], 'old', keep=True) for _ in range(2)]
    require([item[1] for item in old] == [0, 1], 'initial pool cursor differs')
    if udp:
        exchange(suite, old[0][0], 0, 'zero', zero=True, binding=old[0][2])
    suite.metrics('initial-old-bindings')
    suite.backends[0].stop_tcp()
    suite.health(['Unhealthy', 'Healthy'])
    for client, index, source in old:
        exchange(suite, client, index, 'partial-old', binding=source if udp else None)
    for _ in range(4):
        new_business(suite, [False, True], 'only-B')
    suite.metrics('partial-B-only')
    suite.backends[1].stop_tcp()
    suite.health(['Unhealthy', 'Unhealthy'])
    for _ in range(2):
        rejected(suite, 'all-bad')
    for client, index, source in old:
        exchange(suite, client, index, 'all-bad-old', binding=source if udp else None)
    suite.metrics('all-bad-rejections-and-old')
    suite.backends[0].start_tcp()
    suite.health(['Healthy', 'Unhealthy'])
    for _ in range(2):
        new_business(suite, [True, False], 'only-A')
    suite.metrics('only-A-recovered')
    suite.backends[1].start_tcp()
    suite.health(['Healthy', 'Healthy'])
    for _ in range(4):
        new_business(suite, [True, True], 'cursor-rejoin')
    for client, index, source in old:
        exchange(suite, client, index, 'final-old', binding=source if udp else None)
    suite.metrics('both-recovered-current-cursor')
    suite.finish()


def failure(suite):
    suite.backends.append(Backend(suite, 1, False))
    reserved = bound(socket.SOCK_STREAM)
    dead = reserved.getsockname()[1]
    reserved.close()
    suite.action('dead-port-closed', port=dead)
    suite.product = Product(suite, 'tcp', [dead, suite.backends[0].port], 'off')
    suite.product.start()
    suite.health(['disabled', 'disabled'])
    rejected(suite, 'dead-first', business_failure=True)
    suite.cursor = 1
    suite.metrics('first-real-refused')
    new_business(suite, [True, True], 'live-after-failure')
    suite.metrics('next-client-live')
    rejected(suite, 'dead-again', business_failure=True)
    suite.cursor = 1
    suite.metrics('second-real-refused')
    raw = suite.product.logs()
    require(raw.count('errno=111') == 2, 'two real ECONNREFUSED events required')
    require(suite.backends[0].probes == 0, 'health off must not probe')
    suite.finish()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('program')
    parser.add_argument('evidence')
    parser.add_argument('scenario', choices=['tcp', 'udp', 'failure'])
    parser.add_argument('--fault', default='', choices=['', 'wrong-backend', 'child-exit', 'snapshot-seq', 'thread-error', 'wrong-backend-cleanup'])
    args = parser.parse_args()
    suite = Suite(args.program, args.evidence, args.scenario, args.fault)
    primary = None
    try:
        if args.scenario == 'failure':
            failure(suite)
        else:
            pool(suite)
    except Exception as error:
        primary = str(error)
    finally:
        try:
            cleanup_errors = suite.cleanup()
        except Exception as error:
            cleanup_errors = ['cleanup failure: ' + str(error)]
    ok = primary is None and not cleanup_errors
    suite.write('result.json', {'scenario': args.scenario, 'fault': args.fault, 'passed': ok, 'primary': primary, 'cleanup_errors': cleanup_errors})
    print(('PASS' if ok else 'FAIL') + f' S3 {args.scenario}: ' + str(primary or cleanup_errors or 'independent ledger matched'), flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
