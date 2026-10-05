#!/usr/bin/env python3
"""Pure schema/clock/count/identity negatives; no sockets or root required."""
import copy
import gzip
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
import threading
from unittest.mock import patch

import v12_benchmark_measure as measure
from v12_benchmark_measure import atomic_json, packet, HEADER
from v12_benchmark_data import digest, load, matrix, quantiles, reduce_run, summarize, validate_run


def fixture():
    spec = dict(matrix()[0], rate=100, window_s=1, warmup_s=.2)
    identity = {'product': {'commit': '2dd2c98283773925fe179bc58d9e93ceac148ab7'}}
    counts = dict(target=100, attempted=80, sent=70, send_errors=10, missed_slots=20, unique_received=60, timeout=10,
                  late=2, duplicate=1, corrupt=0, wrong_source=0)
    rows = []
    for ns, cpu in [(900000000, 10), (1500000000, 20), (2100000000, 30)]:
        rows.append(dict(ns=ns, pids={'generator': dict(pid=10, ns=ns, starttime=3, utime=cpu, stime=0, rss_bytes=1000, fds=2)},
                         system=dict(ns=ns, stat={'cpu': [cpu, 0, cpu, cpu, 0, 0, cpu, 0, 0, 0]}, softirqs={'NET_RX': [cpu], 'NET_TX': [cpu]})))
    nonce = '12' * 16
    examples = [dict(client=i, source=['198.19.0.100', 39001], backend=0, header_hex=packet(nonce, 0, i, i, 64)[:HEADER.size].hex()) for i in range(16)]
    run = dict(nonce=nonce, spec=spec, identity=digest(identity), valid=True, error=None, cleanup_errors=[], ticks=100,
               traffic=dict(counts=counts, start_ns=1000000000, end_ns=2000000000, finish_ns=2050000000,
                            rtt=[dict(client=0, seq=0, send_ns=1000000000, receive_ns=1000001000, rtt_ns=1000)],
                            backend_distribution=[60, 0], seconds=[dict(sent=70, received=60)]),
               resources=rows, backends=[dict(measurement_received=65, measurement_replied=61), dict(measurement_received=0, measurement_replied=0)],
               preflight=dict(reply_source_verified=True, received=16, reply_examples=examples), events=[], cleanup=dict(remaining=[], owned_reaped=True))
    return identity, run


class PacingClock:
    def __init__(self):
        self.now = 1_000_000_000

    def monotonic_ns(self):
        return self.now

    def sleep(self, duration):
        self.now += max(1, int(duration * 1e9))


class PacingSocket:
    destination = ('198.19.0.100', 39001)

    def __init__(self, clock, sends):
        self.clock, self.sends, self.pending = clock, sends, []

    def sendto(self, data, destination):
        sequence = HEADER.unpack_from(data)[-1]
        self.sends.append((sequence, self.clock.now))
        self.pending.append((data[:-1] + b'\1', destination))

    def recvfrom(self, size):
        if not self.pending:
            raise BlockingIOError()
        return self.pending.pop(0)


def pacing_run(rate, seconds, early=False, late_ns=0):
    clock, sends = PacingClock(), []
    start = clock.now
    sockets = [PacingSocket(clock, sends) for _ in range(16)]
    wakes = []
    def select(read, _write, _error, timeout):
        ready = [sock for sock in read if sock.pending]
        if ready:
            return ready, [], []
        if late_ns and not wakes:
            clock.now += late_ns
        else:
            clock.sleep(timeout / 2 if early else timeout)
        wakes.append(clock.now)
        return [], [], []
    with patch.object(measure.time, 'monotonic_ns', clock.monotonic_ns), patch.object(measure.time, 'sleep', clock.sleep), patch.object(measure.select, 'select', select):
        result = measure.traffic(sockets, '12' * 16, 2, 64, rate, seconds, start)
    return start, sends, result, wakes


class Tests(unittest.TestCase):
    def test_pacing_immediate_replies(self):
        start, sends, result, _ = pacing_run(100, .1)
        self.assertEqual([seq for seq, _ in sends], list(range(10)))
        self.assertEqual([ns - start for _, ns in sends], [i * 10_000_000 for i in range(10)])
        self.assertEqual(result['counts']['unique_received'], 10)
        self.assertEqual(result['counts']['missed_slots'], 0)

    def test_pacing_early_select_wakes(self):
        start, sends, result, wakes = pacing_run(100, .1, early=True)
        self.assertGreater(len(wakes), 10)
        for seq, ns in sends:
            self.assertGreaterEqual(ns, start + seq * 10_000_000)
        self.assertEqual(result['counts']['sent'], 10)

    def test_pacing_late_slots_do_not_burst(self):
        start, sends, result, _ = pacing_run(100, .1, late_ns=35_000_000)
        self.assertEqual([seq for seq, _ in sends], [0, 3, 4, 5, 6, 7, 8, 9])
        self.assertEqual(result['counts']['missed_slots'], 2)
        self.assertEqual(result['counts']['target'], result['counts']['sent'] + result['counts']['missed_slots'])
        self.assertEqual(sends[1][1] - start, 35_000_000)
        self.assertEqual(sends[2][1] - start, 40_000_000)

    def test_pacing_client_rotation_and_window_boundary(self):
        start, sends, result, _ = pacing_run(100, .32)
        self.assertEqual([seq for seq, _ in sends], list(range(32)))
        self.assertEqual(result['counts']['unique_received'], 32)
        self.assertTrue(all(start <= ns < start + 320_000_000 for _, ns in sends))
        # Receiver validation also enforces sequence modulo 16 socket identity.
        self.assertEqual(sends[-1][1] - start, 310_000_000)


    def test_atomic_handoff_during_partial_write(self):
        for name, value in [('go.json', 155716290424138), ('traffic.json', {'counts': {'sent': 20}, 'samples': list(range(100))})]:
            with tempfile.TemporaryDirectory(prefix='v12 atomic space ') as directory:
                path = Path(directory) / name
                half_written, allow_finish = threading.Event(), threading.Event()
                errors = []
                def delayed_dump(obj, stream):
                    text = json.dumps(obj)
                    stream.write(text[:len(text) // 2])
                    stream.flush()
                    half_written.set()
                    if not allow_finish.wait(2):
                        raise RuntimeError('test writer release timeout')
                    stream.write(text[len(text) // 2:])
                def writer():
                    try:
                        atomic_json(path, value)
                    except BaseException as exc:
                        errors.append(exc)
                with patch('v12_benchmark_measure.json.dump', side_effect=delayed_dump):
                    thread = threading.Thread(target=writer)
                    thread.start()
                    try:
                        self.assertTrue(half_written.wait(2))
                        self.assertGreater(path.with_name(name + '.pending').stat().st_size, 0)
                        # Even a reader scheduled between both write chunks cannot
                        # observe the handoff name until close + atomic replace.
                        self.assertFalse(path.exists())
                    finally:
                        allow_finish.set()
                        thread.join(2)
                self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [])
                self.assertEqual(json.loads(path.read_text()), value)
                self.assertFalse(path.with_name(name + '.pending').exists())

    def test_matrix(self):
        m = matrix()
        self.assertEqual(len(m), 126)
        self.assertEqual(sum(x['kind'] == 'data' for x in m), 108)
        self.assertEqual(sum(x['kind'] == 'control' for x in m), 18)
        self.assertEqual(len({json.dumps(x, sort_keys=True) for x in m}), 126)
        for r, first in [(0, 'direct'), (1, 'static-generic'), (2, 'runtime-generic')]:
            self.assertEqual(next(x for x in m if x['repeat'] == r)['path'], first)

    def test_arithmetic(self):
        identity, run = fixture()
        validate_run(run, identity)
        result = reduce_run(run)
        self.assertEqual(result['pps'], 60)
        self.assertAlmostEqual(result['loss'], 1 / 7)
        self.assertEqual(result['rtt']['p50_us'], 1)
        self.assertIsNone(result['rtt']['p99_us'])
        self.assertAlmostEqual(result['process']['generator']['one_core_pct'], 100 / 6)
        samples = [dict(rtt_ns=i * 1000) for i in range(1, 101)]
        self.assertEqual(quantiles(samples)['p99_us'], 99)
        self.assertIsNone(quantiles([])['p50_us'])

    def test_semantic_damage_after_rehash(self):
        identity, original = fixture()
        mutations = [lambda r: r['traffic']['counts'].__setitem__('sent', 71),
                     lambda r: r['traffic']['rtt'][0].__setitem__('rtt_ns', 2000),
                     lambda r: r['resources'][1]['pids']['generator'].__setitem__('ns', 1),
                     lambda r: r['resources'][1]['pids']['generator'].__setitem__('utime', 1),
                     lambda r: r.__setitem__('identity', '0' * 64),
                     lambda r: r['traffic']['seconds'][0].__setitem__('received', 59),
                     lambda r: r['cleanup'].__setitem__('remaining', [123])]
        for mutation in mutations:
            run = copy.deepcopy(original)
            mutation(run)
            package = dict(schema_version=1, identity=identity, formal=False, manifest=[run['spec']], runs=[dict(sha256=digest(run), data=run)])
            with self.assertRaises(ValueError):
                summarize(package)
        package = dict(schema_version=1, identity=identity, formal=True, manifest=matrix(), runs=[])
        with self.assertRaises(ValueError):
            summarize(package)

    def test_duplicate_single_run_is_actual_damage(self):
        identity, run = fixture()
        package = dict(schema_version=1, identity=identity, formal=False, manifest=[run['spec']],
                       runs=[dict(sha256=digest(run), data=run)])
        summarize(package)
        package['runs'].append(copy.deepcopy(package['runs'][0]))
        self.assertEqual(len(package['runs']), 2)
        with self.assertRaisesRegex(ValueError, 'matrix missing/duplicate'):
            summarize(package)

    def test_raw_identity_and_control_association(self):
        identity, original = fixture()
        for mutate in [lambda r: r.__setitem__('nonce', '34' * 16),
                       lambda r: r['preflight']['reply_examples'][0].__setitem__('header_hex', packet(r['nonce'], 0, 1, 0, 64)[:HEADER.size].hex()),
                       lambda r: r['preflight']['reply_examples'][0].__setitem__('header_hex', packet(r['nonce'], 1, 0, 0, 64)[:HEADER.size].hex())]:
            run = copy.deepcopy(original)
            mutate(run)
            with self.assertRaisesRegex(ValueError, 'association'):
                validate_run(run, identity)
        run = copy.deepcopy(original)
        run['spec'].update(path='runtime-generic', scene='reorder', kind='control')
        run['traffic']['backend_distribution'] = [30, 30]
        run['preflight'].update(active=2, healthy=2, generation=2, macs=['a', 'b'])
        import hashlib
        content = 'schema=1\n'
        checksum = hashlib.sha256(content.encode()).hexdigest()
        run['events'] = [dict(type='config', ns=1, content=content, sha256=checksum)]
        for i in range(4):
            generation = i + 3
            dispatch = 1000000000 + (i + 1) * 200000000
            run['events'] += [dict(type='hup', planned_ns=dispatch, ns=dispatch, confirmed_ns=dispatch + 2,
                                   config_sha256=checksum, active=2, generation=generation, status='applied',
                                   macs=['b', 'a'] if i % 2 == 0 else ['a', 'b']),
                              dict(type='output', ns=dispatch + 1, line=f'XDP_PUBLISH status=applied reason=reload generation={generation} active_backends=2'),
                              dict(type='output', ns=dispatch + 2, line=f'XDP_RELOAD status=applied configured_backends=2 generation={generation} active_backends=2')]
        validate_run(run, identity)
        for find, replacement in [('generation=3', 'generation=9'), ('active_backends=2', 'active_backends=1')]:
            damaged = copy.deepcopy(run)
            damaged['events'][3]['line'] = damaged['events'][3]['line'].replace(find, replacement)
            with self.assertRaisesRegex(ValueError, 'association'):
                validate_run(damaged, identity)
        damaged = copy.deepcopy(run)
        damaged['events'][2]['line'] = damaged['events'][2]['line'].replace('generation=3', 'generation=9')
        with self.assertRaisesRegex(ValueError, 'publication association'):
            validate_run(damaged, identity)

    def test_truncated_and_limit(self):
        import v12_benchmark_data as module
        with tempfile.TemporaryDirectory(prefix='v12 schema space ') as temp:
            path = Path(temp) / 'x.gz'
            path.write_bytes(gzip.compress(b'{}')[:-3])
            with self.assertRaises((EOFError, OSError)):
                load(path)
            path.write_bytes(gzip.compress(b' ' * 65))
            old = module.LIMIT
            module.LIMIT = 64
            try:
                with self.assertRaises(ValueError):
                    load(path)
            finally:
                module.LIMIT = old


def package_negatives(path):
    original = load(path)
    summarize(original)
    mutations = {
        'nonce': lambda p: p['runs'][0]['data'].__setitem__('nonce', '00' * 16),
        'client_header': lambda p: p['runs'][0]['data']['preflight']['reply_examples'][0].__setitem__('header_hex', packet(p['runs'][0]['data']['nonce'], 0, 1, 0, 64)[:HEADER.size].hex()),
        'counter': lambda p: p['runs'][0]['data']['traffic']['counts'].__setitem__('sent', p['runs'][0]['data']['traffic']['counts']['sent'] + 1),
        'rtt': lambda p: p['runs'][0]['data']['traffic']['rtt'][0].__setitem__('rtt_ns', -1),
        'cpu_clock': lambda p: p['runs'][0]['data']['resources'][1]['pids']['generator'].__setitem__('ns', 1),
        'identity': lambda p: p['runs'][0]['data'].__setitem__('identity', '0' * 64),
        'missing': lambda p: p['runs'].pop(),
        'duplicate': lambda p: p['runs'].append(copy.deepcopy(p['runs'][0])),
    }
    control_index = next((i for i, run in enumerate(original['runs']) if run['data']['spec']['scene'] != 'steady'), None)
    if control_index is not None:
        def damage_confirmation(package):
            events = package['runs'][control_index]['data']['events']
            line = next(e for e in events if e['type'] == 'output' and e['line'].startswith('XDP_RELOAD '))
            import re
            line['line'] = re.sub(r'generation=\d+', 'generation=999999', line['line'])
        mutations['raw_generation'] = damage_confirmation
    for name, mutate in mutations.items():
        package = copy.deepcopy(original)
        mutate(package)
        for run in package['runs']:
            run['sha256'] = digest(run['data'])
        try:
            summarize(package)
        except ValueError:
            pass
        else:
            raise AssertionError('semantic damage accepted after rehash: ' + name)
    print(json.dumps(dict(package=str(path), semantic_negatives=list(mutations), passed=len(mutations))))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--package':
        package_negatives(Path(sys.argv[2]))
    else:
        unittest.main()
