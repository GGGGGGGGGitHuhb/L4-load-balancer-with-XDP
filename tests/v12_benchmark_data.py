"""V1.2 portable measurement schema, semantic validation and pure reduction."""
import gzip
import hashlib
import json
import math
import re

from v12_benchmark_measure import HEADER, MAGIC
import statistics
from pathlib import Path

SCHEMA = 1
LIMIT = 128 * 1024 * 1024
RUN_LIMIT = 8 * 1024 * 1024
PATHS = ['direct', 'proxy', 'static-generic', 'static-native', 'runtime-generic', 'runtime-native']


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def matrix():
    rows = []
    for repeat in range(3):
        for payload in (64, 1024):
            for rate in (1000, 10000, 50000):
                shift = repeat * 2
                for path in PATHS[shift:] + PATHS[:shift]:
                    rows.append(dict(path=path, payload=payload, rate=rate, repeat=repeat, scene='steady', kind='data'))
        scenes = ['steady', 'unchanged', 'reorder']
        for mode in ('generic', 'native'):
            for scene in scenes[repeat:] + scenes[:repeat]:
                rows.append(dict(path='runtime-' + mode, payload=256, rate=10000, repeat=repeat, scene=scene, kind='control'))
    return [dict(row, run_id=f'{i:03d}', clients=16, warmup_s=2, window_s=10, timeout_s=1) for i, row in enumerate(rows)]


def quantiles(samples):
    values = sorted(row['rtt_ns'] / 1000 for row in samples)
    return {key: (values[math.ceil(p * len(values)) - 1] if values and (key != 'p99_us' or len(values) >= 100) else None)
            for key, p in [('p50_us', .5), ('p95_us', .95), ('p99_us', .99)]}


def process_cpu(rows, ticks):
    names = rows[0]['pids']
    result = {}
    for name, first in names.items():
        last = rows[-1]['pids'][name]
        require(last['pid'] == first['pid'] and last['starttime'] == first['starttime'], 'PID reuse')
        dt = (last['ns'] - first['ns']) / 1e9
        cpu = ((last['utime'] + last['stime']) - (first['utime'] + first['stime'])) / ticks
        require(dt > 0 and cpu >= 0 and cpu <= dt * 256, 'CPU clock/range')
        result[name] = dict(cpu_s=cpu, interval_s=dt, one_core_pct=cpu / dt * 100,
                            rss_peak_bytes=max(r['pids'][name]['rss_bytes'] for r in rows),
                            fd_peak=max(r['pids'][name]['fds'] for r in rows))
    return result


def system_cpu(first, last, ticks):
    result = {}
    require(last['ns'] > first['ns'], 'system clock')
    for name, a in first['stat'].items():
        b = last['stat'][name]
        d = [y - x for x, y in zip(a, b)]
        require(len(d) >= 8 and all(x >= 0 for x in d[:8]), 'system CPU counters')
        # guest is already included in user/nice; irq/softirq are busy subsets.
        result[name] = dict(busy_s=sum(d[i] for i in (0, 1, 2, 5, 6, 7)) / ticks,
                            system_s=d[2] / ticks, softirq_s=d[6] / ticks)
    return dict(interval_s=(last['ns'] - first['ns']) / 1e9, cpus=result,
                net_softirqs={k: [y - x for x, y in zip(first['softirqs'][k], last['softirqs'][k])]
                              for k in first['softirqs']})


def validate_run(run, identity):
    require(len(encoded(run)) <= RUN_LIMIT, 'run size limit')
    require(run['identity'] == digest(identity), 'wrong product/tool identity')
    require(run['valid'] and not run['error'] and not run['cleanup_errors'], 'failed run')
    s, t = run['spec'], run['traffic']
    c = t['counts']
    require(t['end_ns'] - t['start_ns'] == int(s['window_s'] * 1e9), 'traffic clock')
    require(t['start_ns'] <= t['finish_ns'] <= t['end_ns'] + int((s['timeout_s'] + .5) * 1e9), 'drain clock')
    require(all(type(v) is int and v >= 0 for v in c.values()), 'counter range')
    require(c['target'] == int(s['rate'] * s['window_s']), 'target')
    require(c['target'] == c['attempted'] + c['missed_slots'], 'attempted/missed identity')
    require(c['attempted'] == c['sent'] + c['send_errors'], 'send identity')
    require(c['sent'] == c['unique_received'] + c['timeout'], 'receive identity')
    require(c['corrupt'] == 0 and c['wrong_source'] == 0, 'corrupt/source')
    require(sum(t['backend_distribution']) == c['unique_received'], 'backend distribution')
    require(c['unique_received'] > 0, 'no verified replies')
    require(s['path'] == 'direct' or all(x > 0 for x in t['backend_distribution']), 'two backend distribution')
    require(sum(row['sent'] for row in t['seconds']) == c['sent'], 'per-second sends')
    require(sum(row['received'] for row in t['seconds']) == c['unique_received'], 'per-second replies')
    require(len(t['seconds']) == math.ceil(s['window_s']), 'second rows')
    seen = set()
    for row in t['rtt']:
        require(row['seq'] % 100 == 0 and 0 <= row['seq'] < c['target'], 'RTT sequence')
        require(row['client'] == row['seq'] % 16 and row['seq'] not in seen, 'RTT identity')
        require(t['start_ns'] <= row['send_ns'] < t['end_ns'], 'RTT send clock')
        require(row['rtt_ns'] == row['receive_ns'] - row['send_ns'] and 0 <= row['rtt_ns'] < s['timeout_s'] * 1e9, 'RTT clock')
        seen.add(row['seq'])
    require(len(t['rtt']) <= c['unique_received'], 'RTT count')
    resource = run['resources']
    require(len(resource) >= 2, 'resource barriers')
    require(resource[0]['ns'] <= t['start_ns'] and resource[-1]['ns'] >= t['end_ns'], 'resource window barriers')
    for previous, current in zip(resource, resource[1:]):
        require(previous['ns'] < current['ns'], 'resource clock')
        require(current['ns'] - previous['ns'] < 1e9, 'resource sampling gap')
        require(current['pids'].keys() == previous['pids'].keys(), 'process set')
        for name, row in current['pids'].items():
            p = previous['pids'][name]
            require(row['starttime'] == p['starttime'] and row['pid'] == p['pid'], 'PID reuse')
            require(row['ns'] > p['ns'] and row['utime'] >= p['utime'] and row['stime'] >= p['stime'], 'PID CPU clock')
            require(row['rss_bytes'] > 0 and row['fds'] >= 0, 'PID resources')
    process_cpu(resource, run['ticks'])
    system_cpu(resource[0]['system'], resource[-1]['system'], run['ticks'])
    for backend in run['backends']:
        require(backend['measurement_received'] >= backend['measurement_replied'] >= 0, 'backend counters')
    require(sum(b['measurement_replied'] for b in run['backends']) >= c['unique_received'], 'backend delivery')
    require(run['preflight']['reply_source_verified'] and run['preflight']['received'] == 16, 'preflight')
    require(isinstance(run['nonce'], str) and re.fullmatch('[0-9a-f]{32}', run['nonce']), 'run nonce')
    examples = run['preflight']['reply_examples']
    require(len(examples) == 16 and [e['client'] for e in examples] == list(range(16)), 'preflight examples')
    require(all(e['source'] == ['198.19.0.100', 39001] and e['backend'] in (0, 1) for e in examples), 'preflight reply source')
    for example in examples:
        header = bytes.fromhex(example['header_hex'])
        require(len(header) == HEADER.size, 'preflight header length')
        magic, nonce, phase, client, sequence = HEADER.unpack(header)
        require(magic == MAGIC and nonce.hex() == run['nonce'] and phase == 0 and
                client == example['client'] and sequence == client, 'preflight nonce/client/run association')
    events = run['events']
    configs = [e for e in events if e['type'] == 'config']
    for event in configs:
        require(hashlib.sha256(event['content'].encode()).hexdigest() == event['sha256'], 'config SHA')
    hup = [e for e in events if e['type'] == 'hup']
    if s['scene'] == 'steady':
        require(not hup, 'steady HUP')
    else:
        require(len(hup) == 4, 'four HUPs')
        for i, event in enumerate(hup):
            require(event['planned_ns'] == t['start_ns'] + int((i + 1) * s['window_s'] / 5 * 1e9), 'planned HUP clock')
            require(event['planned_ns'] <= event['ns'] < t['end_ns'], 'dispatch HUP clock')
            require(event['ns'] <= event['confirmed_ns'] < t['end_ns'] + 1e9, 'confirmation clock')
            require(event['active'] == 2, 'control active count')
            require(any(c['sha256'] == event['config_sha256'] and c['ns'] <= event['ns'] for c in configs), 'control config association')
            expected_macs = run['preflight']['macs'][::-1] if s['scene'] == 'reorder' and i % 2 == 0 else run['preflight']['macs']
            require(event['macs'] == expected_macs, 'control complete configuration')
            lines = [e for e in events if e['type'] == 'output' and e['ns'] == event['confirmed_ns'] and e['line'].startswith('XDP_RELOAD ')]
            require(len(lines) == 1, 'control raw confirmation')
            tokens = lines[0]['line'].split()[1:]
            require(all(token.count('=') == 1 for token in tokens), 'control raw fields')
            fields = dict(token.split('=') for token in tokens)
            require(len(fields) == len(tokens) and fields.get('status') == event['status'] and
                    fields.get('generation') == str(event['generation']) and
                    fields.get('active_backends') == str(event['active']), 'control raw generation association')
            if event['status'] == 'applied':
                require(fields.get('configured_backends') == '2', 'control raw configured count')
                publication = f"XDP_PUBLISH status=applied reason=reload generation={event['generation']} active_backends={event['active']}"
                require(any(e['type'] == 'output' and event['ns'] <= e['ns'] <= event['confirmed_ns'] and e['line'] == publication for e in events), 'control raw publication association')
            require(event['generation'] == run['preflight']['generation'] + (i + 1 if s['scene'] == 'reorder' else 0), 'control generation')
            require(event['status'] == ('applied' if s['scene'] == 'reorder' else 'unchanged'), 'control result')
    if s['path'].startswith('runtime'):
        require(run['preflight']['active'] == 2 and run['preflight']['healthy'] == 2, 'runtime active set')
    require(run['cleanup']['remaining'] == [] and run['cleanup']['owned_reaped'], 'cleanup')


def reduce_run(run):
    s, t = run['spec'], run['traffic']
    c = t['counts']
    return dict(spec=s, counts=c, pps=c['unique_received'] / s['window_s'],
                goodput_mbit_s=c['unique_received'] * s['payload'] * 8 / s['window_s'] / 1e6,
                loss=(c['sent'] - c['unique_received']) / c['sent'] if c['sent'] else None,
                rtt=dict(quantiles(t['rtt']), samples=len(t['rtt']), coverage=len(t['rtt']) / c['unique_received'] if c['unique_received'] else None,
                         p99_insufficient=len(t['rtt']) < 100),
                process=process_cpu(run['resources'], run['ticks']),
                product_cpu=(process_cpu(run['resources'], run['ticks']).get('product')), 
                system=system_cpu(run['resources'][0]['system'], run['resources'][-1]['system'], run['ticks']),
                backend_distribution=t['backend_distribution'], seconds=t['seconds'],
                confirmation_ms=[(e['confirmed_ns'] - e['ns']) / 1e6 for e in run['events'] if e['type'] == 'hup'])


def summarize(package):
    require(package['schema_version'] == SCHEMA, 'schema')
    identity = package['identity']
    require(identity['product']['commit'] == '2dd2c98283773925fe179bc58d9e93ceac148ab7', 'fixed S3 product')
    expected = matrix() if package['formal'] else package['manifest']
    require(package['manifest'] == expected and len(package['runs']) == len(expected), 'matrix missing/duplicate')
    rows, nonces = [], set()
    for spec, item in zip(expected, package['runs']):
        run = item['data']
        require(item['sha256'] == digest(run), 'run checksum')
        require(run['spec'] == spec, 'matrix/order')
        validate_run(run, identity)
        require(run['nonce'] not in nonces, 'duplicate run nonce')
        nonces.add(run['nonce'])
        rows.append(reduce_run(run))
    groups = {}
    for row in rows:
        s = row['spec']
        key = f"{s['kind']}/{s['path']}/{s['payload']}/{s['rate']}/{s['scene']}"
        groups.setdefault(key, []).append(row)
    aggregates = {key: {metric: dict(median=statistics.median(v[metric] for v in values),
                                    min=min(v[metric] for v in values), max=max(v[metric] for v in values))
                        for metric in ['pps', 'goodput_mbit_s', 'loss']} for key, values in groups.items()}
    return dict(schema_version=SCHEMA, identity_sha256=digest(identity), formal=package['formal'], runs=rows, groups=aggregates)


def load(path):
    with gzip.open(path, 'rb') as f:
        data = f.read(LIMIT + 1)
    require(len(data) <= LIMIT, 'package decompression limit')
    return json.loads(data)


def save(package, path):
    data = encoded(package)
    require(len(data) <= LIMIT, 'package size limit')
    with open(path, 'xb') as f:
        f.write(gzip.compress(data, mtime=0))
