#!/usr/bin/env python3
"""独立解析原CLI快照，精确业务nonce与活动期提交量作oracle。"""
import json
import socket
import subprocess
import sys
import time
import health_product_test as f

KEYS = ['schema', 'seq', 'phase', 'protocol', 'uptime_ms', 'sessions_created_total', 'sessions_closed_total', 'sessions_active', 'bytes_c2b_total', 'bytes_b2c_total', 'datagrams_c2b_total', 'datagrams_b2c_total', 'rejected_total', 'dropped_datagrams_total', 'errors_total', 'timeouts_total', 'counter_saturated', 'backends']


def snapshots(product):
    result = []
    for line in product.logs().splitlines(keepends=True):
        if not line.startswith('metrics ') or not line.endswith('\n'):
            continue
        f.check(line.isascii() and len(line) <= 32768, 'ASCII/bounded metrics')
        value = json.loads(line[8:])
        f.check(list(value) == KEYS, 'schema fields/order')
        f.check(value['schema'] == 1 and value['phase'] in ['ready', 'periodic', 'final', 'error'], 'schema/phase')
        f.check(value['protocol'] in ['tcp', 'udp'] and type(value['counter_saturated']) is bool, 'protocol/saturation')
        for key in KEYS:
            if key not in ['phase', 'protocol', 'counter_saturated', 'backends']:
                f.check(type(value[key]) is int and 0 <= value[key] <= 2**64-1, 'uint64 ' + key)
        for index, backend in enumerate(value['backends']):
            f.check(list(backend) == ['index', 'health', 'eligible'] and backend['index'] == index and type(backend['eligible']) is bool, 'backend order/schema')
            f.check(backend['health'] in ['disabled', 'Unknown', 'Healthy', 'Unhealthy'], 'health enum')
            f.check(backend['eligible'] == (backend['health'] in ['disabled', 'Healthy']), 'health eligibility')
        f.check(value['sessions_created_total'] - value['sessions_closed_total'] == value['sessions_active'], 'lifecycle identity')
        f.check(value['seq'] == len(result)+1, 'seq consecutive')
        if result:
            f.check(value['uptime_ms'] >= result[-1]['uptime_ms'], 'monotonic uptime')
        result.append(value)
    return result


def wait_snapshot(product, predicate, why):
    f.until(lambda: any(predicate(s) for s in snapshots(product)), why, 6)
    return next(s for s in reversed(snapshots(product)) if predicate(s))


def first(product, health):
    ready = wait_snapshot(product, lambda s: s['phase'] == 'ready', 'ready snapshot')
    f.check(all(ready[key] == 0 for key in KEYS[5:16]), 'ready counters zero')
    f.check(ready['backends'][0]['health'] == health, 'ready health read-only')
    return ready


def final(product, created, sent, packets=None, rejected=0, dropped=0):
    lines = snapshots(product)
    f.check(len([s for s in lines if s['phase'] in ['final', 'error']]) == 1 and lines[-1]['phase'] == 'final', 'exactly one final')
    s = lines[-1]
    f.check(s['sessions_active'] == 0 and s['sessions_created_total'] == created and s['sessions_closed_total'] == created, 'final cleaned lifecycle')
    f.check(s['bytes_c2b_total'] == sent and s['bytes_b2c_total'] == sent, 'final bytes no duplicate')
    f.check(s['rejected_total'] == rejected and s['dropped_datagrams_total'] == dropped and s['errors_total'] == 0, 'reject/drop/errors exact')
    f.check(s['datagrams_c2b_total'] == (packets or 0) and s['datagrams_b2c_total'] == (packets or 0), 'final packets')
    (product.path / 'parsed-snapshots.json').write_text(json.dumps(lines, indent=2))


def tcp():
    backend = f.Backend()
    p = None
    try:
        p = f.Product('metrics-tcp-off', backend, 'tcp', 'off', 'off')
        time.sleep(1.1)
        f.check(not snapshots(p) and backend.probes == 0, 'metrics off has no lines or probes')
        p.close(); p = None
        p = f.Product('metrics-tcp', backend, 'tcp', 'off', 'stderr')
        first(p, 'disabled')
        with p.stream() as client:
            nonce = b'\x00metrics-tcp-live\xff'
            f.tcp_echo(client, nonce)
            wait_snapshot(p, lambda s: s['phase'] == 'periodic' and s['sessions_active'] == 1 and s['bytes_c2b_total'] == len(nonce) and s['bytes_b2c_total'] == len(nonce), 'TCP live bytes before close')
            client.shutdown(socket.SHUT_WR)
            f.check(client.recv(1) == b'', 'TCP drained EOF')
        wait_snapshot(p, lambda s: s['sessions_closed_total'] == 1, 'normal half-close counted')
        f.check(backend.probes == 0, 'health off no probes')
        p.close(); final(p, 1, len(nonce)); p = None
        p = f.Product('metrics-tcp-health', backend, 'tcp', 'tcp_connect', 'stderr')
        first(p, 'Unknown')
        p.reject_tcp(b'unknown-reject')
        p.state('from=Unknown to=Healthy')
        healthy = wait_snapshot(p, lambda s: s['backends'][0]['health'] == 'Healthy', 'healthy snapshot')
        f.check(healthy['sessions_created_total'] == 0 and healthy['bytes_c2b_total'] == 0 and healthy['rejected_total'] == 1, 'probes excluded and rejection counted')
        with p.stream() as old:
            f.tcp_echo(old, b'before')
            backend.stop_tcp()
            p.state('from=Healthy to=Unhealthy')
            f.tcp_echo(old, b'after')
            p.reject_tcp(b'unhealthy-reject')
            wait_snapshot(p, lambda s: s['backends'][0]['health'] == 'Unhealthy' and s['sessions_active'] == 1 and s['bytes_c2b_total'] == 11 and s['rejected_total'] == 2, 'TCP unhealthy old session metrics')
            p.close(); final(p, 1, 11, rejected=2); p = None
        print('PASS metrics TCP live/half-close/final/health/probe-exclusion/reject schema', flush=True)
    finally:
        if p:
            p.close()
        backend.close()


def udp():
    backend = f.Backend(udp=True)
    p = None
    clients = []
    def client():
        c = f.bind_socket(socket.SOCK_DGRAM); c.settimeout(.3); clients.append(c); return c
    try:
        p = f.Product('metrics-udp', backend, 'udp', 'off', 'stderr')
        first(p, 'disabled')
        one, two = client(), client()
        for c, payload in [(one, b'\x00UDP\xff'), (one, b''), (two, b'next')]:
            f.udp_echo(c, p, payload, backend)
        wait_snapshot(p, lambda s: s['sessions_active'] == 2 and s['bytes_c2b_total'] == 9 and s['bytes_b2c_total'] == 9 and s['datagrams_c2b_total'] == 3 and s['datagrams_b2c_total'] == 3, 'UDP zero packet and bytes before close')
        p.close(); final(p, 2, 9, packets=3); p = None
        backend.start_tcp()
        p = f.Product('metrics-udp-health', backend, 'udp', 'tcp_connect', 'stderr')
        first(p, 'Unknown')
        p.state('from=Unknown to=Healthy')
        old = client()
        f.udp_echo(old, p, b'old-before', backend)
        backend.stop_tcp()
        p.state('from=Healthy to=Unhealthy')
        f.udp_echo(old, p, b'old-after', backend)
        f.udp_drop(client(), p, b'new-rejected', backend)
        wait_snapshot(p, lambda s: s['backends'][0]['health'] == 'Unhealthy' and s['sessions_active'] == 1 and s['bytes_c2b_total'] == 19 and s['bytes_b2c_total'] == 19 and s['rejected_total'] == 1 and s['dropped_datagrams_total'] == 1, 'UDP old flow/ineligible/rejected drop snapshot')
        p.close(); final(p, 1, 19, packets=2, rejected=1, dropped=1); p = None
        print('PASS metrics UDP exact bytes/zero packets/flow cleanup/health old binding/rejected+drop', flush=True)
    finally:
        for c in clients:
            c.close()
        if p:
            p.close()
        backend.close()


def startup_error():
    occupied = f.bind_socket(socket.SOCK_STREAM)
    occupied.listen(1)
    path = f.ROOT / 'metrics-startup-error.conf'
    path.write_text(f'listen=127.0.0.1:{occupied.getsockname()[1]}\nbackend=127.0.0.1:1\nmetrics=stderr\n')
    try:
        run = subprocess.run([f.PROGRAM, '--run', str(path)], capture_output=True, text=True, timeout=3)
        (f.ROOT / 'startup-error.stderr').write_text(run.stderr)
        f.check(run.returncode == 1 and not run.stdout, 'startup error exit/no ready')
        lines = [json.loads(line[8:]) for line in run.stderr.splitlines() if line.startswith('metrics ')]
        f.check(len(lines) == 1 and lines[0]['phase'] == 'error' and lines[0]['errors_total'] == 1 and lines[0]['sessions_active'] == 0, 'startup error one clean tail')
    finally:
        occupied.close()


try:
    if f.MODE in ['all', 'tcp']:
        tcp(); startup_error()
    if f.MODE in ['all', 'udp']:
        udp()
except Exception as error:
    print('FAIL metrics product:', error, file=sys.stderr, flush=True)
    sys.exit(1)
