"""Pure benchmark arithmetic. Rates count verified echo payload once."""
import math
import statistics


def percentiles(values):
    ordered = sorted(values)
    return {'count': len(ordered), **{
        name: ordered[math.ceil(p * len(ordered)) - 1] if ordered else None
        for name, p in [('p50_ns', .50), ('p95_ns', .95), ('p99_ns', .99), ('max_ns', 1)]}}


def rates(sent, received, payload, window_s, elapsed_s):
    return {'sent_per_second': sent / window_s,
            'unique_echoes_per_second': received / elapsed_s,
            'goodput_bytes_per_second': received * payload / elapsed_s,
            'goodput_bits_per_second': received * payload * 8 / elapsed_s,
            'delivery_ratio': received / sent if sent else None,
            'unconfirmed': sent - received,
            'loss_ratio': (sent - received) / sent if sent else None}


def aggregate(results):
    """Do not combine RTT populations or silently discard invalid runs."""
    groups = {}
    for result in results:
        key = result['mode']
        groups.setdefault(key, []).append(result)
    return {key: {'runs': [r['run_id'] for r in values],
                  'all_valid': all(r['valid'] for r in values),
                  'goodput_bytes_per_second': {
                      'median': statistics.median(numbers),
                      'min': min(numbers), 'max': max(numbers)}}
            for key, values in groups.items()
            if (numbers := [r['rates']['goodput_bytes_per_second'] for r in values
                            if 'rates' in r])}
