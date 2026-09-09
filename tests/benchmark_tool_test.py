#!/usr/bin/env python3
"""Deterministic arithmetic, identity, timeout and public-path smoke checks."""
import json
import contextlib
import io
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
import uuid

from benchmark_stats import aggregate, percentiles, rates
from benchmark_traffic import DatagramLedger, packet, tcp_exchange
from benchmark_runner import arguments


class Statistics(unittest.TestCase):
    def test_cross_client_ownership_all_states(self):
        nonce = b'a' * 16
        for state in ['pending', 'duplicate', 'late']:
            with self.subTest(state=state):
                ledger = DatagramLedger(nonce, 1, 64, 2, .1)
                ledger.sent(0, 0, 1_000_000_000)
                if state == 'duplicate':
                    ledger.receive(packet(nonce, 1, 0, 0, 64), 0, 1_010_000_000)
                elif state == 'late':
                    ledger.expire(1_200_000_000)
                before_counts = dict(ledger.values)
                before_states = bytes(ledger.states)
                before_pending = dict(ledger.pending)
                before_samples = list(ledger.samples)
                # Even an overdue pending entry must not expire before rejection.
                with self.assertRaisesRegex(RuntimeError, 'client ownership mismatch'):
                    ledger.receive(packet(nonce, 1, 1, 0, 64), 1, 1_200_000_000)
                self.assertEqual(bytes(ledger.states), before_states)
                self.assertEqual(ledger.pending, before_pending)
                self.assertEqual(ledger.samples, before_samples)
                before_counts['foreign_corrupt'] += 1
                self.assertEqual(ledger.values, before_counts)
                self.assertEqual(len(ledger.owners), len(ledger.states))

    def test_parameter_bounds(self):
        base = ['--program', sys.executable, '--protocol', 'udp', '--output', '/unused']
        defaults = arguments(base)
        self.assertEqual((defaults.clients, defaults.rate, defaults.repeats), (1, 1000, 3))
        for flag, value in [('clients', '0'), ('clients', '65'), ('payload', '63'),
                            ('payload', '4097'), ('warmup', '-1'), ('warmup', '11'),
                            ('duration', '0'), ('duration', '61'), ('duration', 'nan'),
                            ('duration', 'inf'), ('rate', '0'), ('rate', '100001'),
                            ('timeout', '.09'), ('timeout', '5.1'), ('repeats', '0'), ('repeats', '11')]:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                arguments([*base, '--' + flag, value])
            self.assertEqual(raised.exception.code, 2)

    def test_partial_io(self):
        class PartialSocket:
            def __init__(self):
                self.written = bytearray()
                self.read_at = 0

            def settimeout(self, timeout):
                pass

            def send(self, data):
                self.written.extend(data[:3])
                return min(3, len(data))

            def recv(self, count):
                result = self.written[self.read_at:self.read_at + min(count, 2)]
                self.read_at += len(result)
                return result

        sock = PartialSocket()
        tcp_exchange(sock, b'partial-read-and-write', time.monotonic() + 1)
        self.assertEqual(sock.written, b'partial-read-and-write')
        self.assertEqual(sock.read_at, len(sock.written))

    def test_rank(self):
        self.assertIsNone(percentiles([])['p99_ns'])
        self.assertEqual(percentiles([7])['p50_ns'], 7)
        self.assertEqual(percentiles(list(range(1, 101))),
                         {'count': 100, 'p50_ns': 50, 'p95_ns': 95, 'p99_ns': 99, 'max_ns': 100})
        self.assertEqual(percentiles([9, 2, 5])['p50_ns'], 5)

    def test_units_windows_loss(self):
        result = rates(10, 8, 100, 2, 4)
        self.assertEqual(result['sent_per_second'], 5)
        self.assertEqual(result['goodput_bytes_per_second'], 200)
        self.assertEqual(result['goodput_bits_per_second'], 1600)
        self.assertEqual(result['loss_ratio'], .2)
        self.assertIsNone(rates(0, 0, 100, 1, 1)['delivery_ratio'])
        self.assertEqual(aggregate([{'mode': 'direct', 'run_id': str(i), 'valid': True,
                                    'rates': {'goodput_bytes_per_second': n}}
                                   for i, n in enumerate([7, 1, 4])])['direct']['goodput_bytes_per_second'],
                         {'median': 4, 'min': 1, 'max': 7})

    def test_datagram_ledger(self):
        nonce = b'a' * 16
        ledger = DatagramLedger(nonce, 1, 64, 101, .1)
        ledger.sent(0, 0, 1_000_000_000)
        ledger.sent(1, 0, 1_010_000_000)
        ledger.sent(100, 0, 1_020_000_000)
        ledger.receive(packet(nonce, 1, 0, 100, 64), 0, 1_025_000_000)
        ledger.receive(packet(nonce, 1, 0, 0, 64), 0, 1_030_000_000)
        ledger.receive(packet(nonce, 1, 0, 0, 64), 0, 1_035_000_000)
        ledger.receive(packet(nonce, 1, 0, 1, 64), 0, 1_200_000_000)
        ledger.receive(packet(nonce, 0, 0, 9, 64), 0, 1_201_000_000)
        self.assertEqual([ledger.values[k] for k in ['received', 'duplicate', 'late', 'timeout', 'warmup_excluded']], [2, 1, 1, 1, 1])
        self.assertEqual([row['rtt_ns'] for row in ledger.samples], [5_000_000, 30_000_000])
        with self.assertRaisesRegex(RuntimeError, 'corrupt'):
            ledger.receive(packet(b'b' * 16, 1, 0, 0, 64), 0, 1_202_000_000)


def verify(directory, zero_loss=False):
    summary = json.loads((directory / 'summary.json').read_text())
    assert summary['valid'], summary
    for run in summary['runs']:
        path = directory / run['path']
        result = json.loads((path / 'result.json').read_text())
        assert result['valid'] and result['counts']['received'] > 0
        assert result['fixture_fd']['before'] == result['fixture_fd']['after']
        assert result['backend_cleanup']['fd_before'] == result['backend_cleanup']['fd_after']
        for process in result['owned_processes']:
            assert process['reaped'] and not Path(f"/proc/{process['pid']}").exists()
        samples = [json.loads(line)['rtt_ns'] for line in (path / 'rtt-samples.jsonl').read_text().splitlines()]
        values = sorted(samples)
        for name, fraction in [('p50_ns', .5), ('p95_ns', .95), ('p99_ns', .99)]:
            assert result['sampled_rtt'][name] == (values[math.ceil(fraction * len(values)) - 1] if values else None)
        assert result['rates']['goodput_bits_per_second'] == result['rates']['goodput_bytes_per_second'] * 8
        if zero_loss:
            assert result['counts']['timeout'] == 0 and result['counts']['sent'] == result['counts']['received']


def smoke():
    program, root, protocol = sys.argv[2:]
    directory = Path(root) / ('smoke-' + protocol + '-' + uuid.uuid4().hex)
    command = [sys.executable, str(Path(__file__).with_name('benchmark_runner.py')),
               '--program', program, '--protocol', protocol, '--mode', 'paired',
               '--duration', '1', '--warmup', '0', '--repeats', '1', '--output', str(directory)]
    completed = subprocess.run(command, timeout=40, capture_output=True, text=True)
    assert completed.returncode == 0, completed.stderr + completed.stdout
    verify(directory, zero_loss=True)
    print(f'{protocol} smoke PASS: {directory}')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'smoke':
        smoke()
    else:
        unittest.main()
