"""Independent raw arithmetic and bounded portable comparison data package."""
import gzip
import json
import math
from pathlib import Path
import statistics

from v04_benchmark_identity import digest, require, static_manifest, write


def schedule(smoke=False):
    cells = []
    for protocol in (['tcp'] if smoke else ['tcp', 'udp']):
        for clients in ([1] if smoke else [1, 16]):
            for repeat in range(1 if smoke else 3):
                roles = ['baseline', 'candidate'] if repeat % 2 == 0 else ['candidate', 'baseline']
                modes = ['direct', 'proxy'] if repeat % 2 == 0 else ['proxy', 'direct']
                for role in roles:
                    for mode in modes:
                        cells.append(dict(protocol=protocol, clients=clients, round=repeat,
                                          role=role, mode=mode))
    return cells


def parameters(cell, smoke=False):
    return dict(protocol=cell['protocol'], clients=cell['clients'], mode=cell['mode'],
                payload=4096 if cell['protocol'] == 'tcp' else 256,
                rate=10000, warmup=0 if smoke else 1, duration=1 if smoke else 5,
                timeout=1, repeats=1, fault='none')


def close(actual, expected, name):
    require((actual is None and expected is None) or
            (isinstance(actual, (float, int)) and isinstance(expected, (float, int)) and
             math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-9)), name + ' raw mismatch')


def ranks(samples):
    values = sorted(samples)
    return {key: values[math.ceil(len(values) * p) - 1] if values else None
            for key, p in [('p50_ns', .5), ('p95_ns', .95), ('p99_ns', .99), ('max_ns', 1)]}


def delta(before, after):
    return {'absolute': after - before, 'relative_percent': 100 * (after - before) / before if before else None,
            'reason': None if before else 'baseline denominator is zero'}


def raw_metrics(record):
    r, p = record['result'], record['parameters']
    require(r['valid'] and r['exit_code'] == 0 and record['invocation']['returncode'] == 0,
            'failed run cannot summarize')
    require(not r['primary_error'] and not r['cleanup_errors'], 'run cleanup error')
    require(r['fixture_fd']['before'] == r['fixture_fd']['after'], 'fixture fd leak')
    require(r['backend_cleanup']['fd_before'] == r['backend_cleanup']['fd_after'], 'backend fd leak')
    require(all(x['reaped'] for x in r['owned_processes']), 'owned PID not reaped')
    c, t = r['counts'], r['timing']
    require(all(isinstance(x, int) and x >= 0 for x in c.values()), 'invalid counts')
    require(0 < c['received'] <= c['sent'] <= c['attempted'], 'invalid delivery counts')
    require(c['sent'] + c['send_errors'] == c['attempted'], 'attempt/send accounting')
    require(c['timeout'] == c['sent'] - c['received'] and c['foreign_corrupt'] == 0, 'loss/corrupt accounting')
    window = (t['t1_ns'] - t['t0_ns']) / 1e9
    elapsed = (t['drain_end_ns'] - t['t0_ns']) / 1e9
    require(window == p['duration'] and elapsed >= window, 'measurement window')
    close(t['send_window_seconds'], window, 'send window')
    close(t['goodput_elapsed_seconds'], elapsed, 'drain window')
    if p['protocol'] == 'udp':
        require(c['target'] == p['duration'] * p['rate'], 'UDP target')
        require(c['attempted'] + c['missed_slots'] == c['target'], 'UDP pacing accounting')
    require(r['under_target'] == (p['protocol'] == 'udp' and c['sent'] < c['target']), 'under target')
    calculated = {'sent_per_second': c['sent'] / window,
                  'unique_echoes_per_second': c['received'] / elapsed,
                  'goodput_bytes_per_second': c['received'] * p['payload'] / elapsed,
                  'goodput_bits_per_second': c['received'] * p['payload'] * 8 / elapsed,
                  'delivery_ratio': c['received'] / c['sent'], 'unconfirmed': c['sent'] - c['received'],
                  'loss_ratio': (c['sent'] - c['received']) / c['sent']}
    for key, value in calculated.items():
        close(r['rates'][key], value, key)
    rows = record['rtt']
    require(len({(s['client'], s['seq']) for s in rows}) == len(rows), 'duplicate RTT identity')
    require(all(0 <= s['client'] < p['clients'] and s['seq'] >= 0 and s['seq'] % 100 == 0 and s['rtt_ns'] >= 0
                for s in rows), 'RTT identity/value')
    require(len(rows) <= c['received'], 'RTT sample count')
    if p['protocol'] == 'tcp':
        require(c['sent'] == c['received'] and len(rows) >= math.ceil(c['received'] / 100), 'TCP sampling coverage')
    sampled = r['sampled_rtt']
    require(sampled['count'] == len(rows) and sampled['stride'] == 100, 'RTT count/stride')
    require(sampled['p99_insufficient_samples'] == (len(rows) < 100), 'p99 sufficiency')
    close(sampled['coverage'], len(rows) / c['received'], 'RTT coverage')
    for key, value in ranks([s['rtt_ns'] for s in rows]).items():
        close(sampled[key], value, key)
        calculated[key] = value
    expected_roles = {'generator', 'backend'} | ({'product'} if p['mode'] == 'proxy' else set())
    require(set(record['resources']) == expected_roles, 'resource roles')
    if p['mode'] == 'direct':
        require(r['resources']['product']['value'] is None, 'direct product resource must be null')
        calculated['product_cpu_percent'] = None
        calculated['product_rss_bytes'] = None
        calculated['product_fd'] = None
    for name, samples in record['resources'].items():
        require(len(samples) >= 2, 'missing resource samples')
        require(all(b['monotonic_ns'] > a['monotonic_ns'] and b['cpu_seconds'] >= a['cpu_seconds']
                    for a, b in zip(samples, samples[1:])), 'resource sample order')
        first, last = samples[0], samples[-1]
        require(first['monotonic_ns'] <= t['t0_ns'] and last['monotonic_ns'] >= t['drain_end_ns'], 'resource window coverage')
        seconds = (last['monotonic_ns'] - first['monotonic_ns']) / 1e9
        cpu = last['cpu_seconds'] - first['cpu_seconds']
        values = dict(start_ns=first['monotonic_ns'], end_ns=last['monotonic_ns'], wall_seconds=seconds,
                      cpu_seconds=cpu, cpu_percent_one_core=100 * cpu / seconds,
                      sampled_peak_rss_bytes=max(x['rss_bytes'] for x in samples),
                      sampled_peak_fd=max(x['fd_count'] for x in samples), sample_count=len(samples))
        for key, value in values.items():
            close(r['resources'][name][key], value, name + '/' + key)
        calculated[name + '_cpu_percent'] = values['cpu_percent_one_core']
        calculated[name + '_rss_bytes'] = values['sampled_peak_rss_bytes']
        calculated[name + '_fd'] = values['sampled_peak_fd']
    calculated.update(missed_slots=c['missed_slots'], loss=c['sent'] - c['received'],
                      duplicate=c['duplicate'], late=c['late'], rtt_count=len(rows), under_target=r['under_target'])
    return calculated


def summarize(data):
    require(data['schema'] == 1, 'package schema')
    expected = schedule(data['smoke'])
    require(data['schedule'] == expected, 'schedule mismatch')
    require(len(data['runs']) == len(expected), 'missing/extra matrix cells')
    manifests = data['manifests']
    require(set(manifests) == {'baseline', 'candidate'}, 'missing product identity')
    for role, manifest in manifests.items():
        static_manifest(manifest)
        require(manifest['role'] == role, 'manifest role')
    require(manifests['baseline']['versions'] == manifests['candidate']['versions'], 'compiler version mismatch')
    require(manifests['baseline']['compiler_path'] == manifests['candidate']['compiler_path'], 'compiler path mismatch')
    require(manifests['baseline']['effective_flags'] == manifests['candidate']['effective_flags'], 'effective build flags mismatch')
    ids, groups, metrics, last_end = set(), {}, [], 0
    for index, (cell, record) in enumerate(zip(expected, data['runs'])):
        require(record['cell'] == cell and record['parameters'] == parameters(cell, data['smoke']), 'cell/parameter mismatch')
        ident = record['product_identity']
        require(ident == {'role': cell['role'], 'manifest_sha256': digest(manifests[cell['role']]),
                          'tool_sha256': data['tool_sha256']}, 'product/tool identity mismatch')
        r, env, invocation = record['result'], record['environment'], record['invocation']
        require(r['mode'] == cell['mode'] and r['protocol'] == cell['protocol'], 'result cell mismatch')
        require(r['run_id'] not in ids, 'duplicate run ID')
        ids.add(r['run_id'])
        require(invocation['start_ns'] >= last_end and invocation['end_ns'] > invocation['start_ns'], 'overlapping execution')
        last_end = invocation['end_ns']
        require(env['binary_sha256']['value'] == manifests[cell['role']]['binary_sha256'], 'run binary identity')
        require(all(env['parameters'][k] == v for k, v in record['parameters'].items()), 'runner parameters mismatch')
        require(all(data['tool_sha256'][k] == v for k, v in env['tool_sha256'].items()), 'runner tool mismatch')
        require('health_check=off\nmetrics=off\n' in record['configuration'], 'configuration controls')
        values = raw_metrics(record)
        metrics.append(dict(index=index, cell=cell, run_id=r['run_id'], **values))
        key = '/'.join(str(cell[k]) for k in ('role', 'protocol', 'clients', 'mode'))
        groups.setdefault(key, []).append(values)
    summaries = {}
    for key, rows in groups.items():
        summaries[key] = {name: {'values': [r[name] for r in rows],
                                  'median': statistics.median([r[name] for r in rows]) if rows[0][name] is not None else None,
                                  'min': min(r[name] for r in rows) if rows[0][name] is not None else None,
                                  'max': max(r[name] for r in rows) if rows[0][name] is not None else None}
                          for name in rows[0]}
    comparisons = []
    for row in metrics:
        if row['cell']['role'] == 'candidate' and row['cell']['mode'] == 'proxy':
            base = next(x for x in metrics if x['cell'] == dict(row['cell'], role='baseline'))
            comparisons.append({'cell': row['cell'], 'metrics': {
                key: delta(base[key], row[key]) for key in ['goodput_bytes_per_second', 'p50_ns', 'p95_ns', 'p99_ns',
                                                            'product_cpu_percent', 'product_rss_bytes']
                if base[key] is not None and row[key] is not None}})
    return dict(schema=1, valid=True, formal=not data['smoke'], runs=metrics, groups=summaries,
                comparisons=comparisons, tool_sha256=data['tool_sha256'],
                product_manifests={k: digest(v) for k, v in manifests.items()})


def save_package(path, data):
    envelope = {'payload': data, 'sha256': digest(data),
                'run_sha256': [digest(r) for r in data['runs']]}
    Path(path).write_bytes(gzip.compress(json.dumps(envelope, separators=(',', ':'), ensure_ascii=False).encode(), mtime=0))


def load_package(path):
    with gzip.open(path, 'rb') as stream:
        raw = stream.read(128 * 1024 * 1024 + 1)
    require(len(raw) <= 128 * 1024 * 1024, 'package exceeds 128 MiB limit')
    envelope = json.loads(raw)
    data = envelope['payload']
    require(envelope['sha256'] == digest(data), 'package SHA mismatch')
    require(envelope['run_sha256'] == [digest(r) for r in data['runs']], 'run SHA mismatch')
    return data
