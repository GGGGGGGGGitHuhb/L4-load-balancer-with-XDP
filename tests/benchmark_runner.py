#!/usr/bin/env python3
"""Reproducible bounded TCP/UDP benchmark. Only standard-library tool dependencies."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import uuid

from benchmark_environment import Resources, environment
from benchmark_stats import aggregate, percentiles, rates
from benchmark_traffic import STRIDE, tcp_phase, udp_phase


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')


def utc():
    return datetime.now(timezone.utc).isoformat()


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--protocol', choices=['tcp', 'udp'], required=True)
    parser.add_argument('--mode', choices=['direct', 'proxy', 'paired'], default='paired')
    parser.add_argument('--output', type=Path, required=True, help='must not exist')
    for name, default, kind in [('clients', 1, int), ('payload', 256, int),
                                ('warmup', 1, float), ('duration', 5, float),
                                ('rate', 1000, int), ('timeout', 1, float), ('repeats', 3, int)]:
        parser.add_argument('--' + name, type=kind, default=default)
    parser.add_argument('--fault', choices=['none', 'corrupt', 'background', 'product-exit',
                                          'evidence', 'permission', 'cleanup', 'resource',
                                          'tcp-partial', 'udp-disorder', 'udp-cross-client', 'resource-evidence'], default='none',
                        help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    for name, low, high in [('clients', 1, 64), ('payload', 64, 4096), ('warmup', 0, 10),
                            ('duration', 1, 60), ('rate', 1, 100000), ('timeout', .1, 5), ('repeats', 1, 10)]:
        if not low <= getattr(args, name) <= high:
            parser.error(f'{name} must be within {low}..{high}')
    args.program = args.program.resolve()
    args.output = args.output.resolve()
    if not args.program.is_file() or not os.access(args.program, os.X_OK):
        parser.error('program must be an executable file')
    return args


def run_one(args, mode, index, order):
    directory = args.output / f'{index:02d}-{mode}'
    directory.mkdir()
    result = {'schema_version': 1, 'run_id': str(uuid.uuid4()), 'valid': False,
              'mode': mode, 'protocol': args.protocol, 'repeat': index,
              'pair_order': order, 'utc_start': utc(), 'primary_error': None,
              'cleanup_errors': [], 'owned_processes': [], 'exit_code': 1}
    processes = []
    logs = []
    sockets = []
    sampler = None
    product = None
    baseline_fd = len(os.listdir('/proc/self/fd'))
    hard_seconds = 15 + args.warmup + args.duration + args.timeout + 2 * args.clients / 10
    result['hard_timeout_seconds'] = hard_seconds
    hard_end = time.monotonic() + hard_seconds

    def alarm(*unused):
        raise TimeoutError('internal hard timeout exceeded')

    prior_alarm = signal.signal(signal.SIGALRM, alarm)
    def interrupted(*unused):
        raise InterruptedError('benchmark interrupted by SIGTERM')

    prior_term = signal.signal(signal.SIGTERM, interrupted)
    signal.setitimer(signal.ITIMER_REAL, hard_seconds)

    def check():
        if time.monotonic() >= hard_end:
            raise TimeoutError('run deadline exceeded')
        for name, process in processes:
            code = process.poll()
            if code is not None:
                raise RuntimeError(f'{name} unexpected exit {code}')
        if sampler and sampler.errors:
            raise RuntimeError('background resource sampler: ' + '; '.join(sampler.errors))

    def start(name, argv):
        stdout = (directory / f'{name}.stdout.log').open('w')
        stderr = (directory / f'{name}.stderr.log').open('w')
        logs.extend([stdout, stderr])
        process = subprocess.Popen(argv, stdout=stdout, stderr=stderr)
        processes.append((name, process))
        result['owned_processes'].append({'name': name, 'pid': process.pid, 'argv': argv})
        return process

    def wait(predicate):
        deadline = min(hard_end, time.monotonic() + 5)
        while not predicate():
            check()
            if time.monotonic() >= deadline:
                raise TimeoutError('ready handshake exceeded 5 seconds')
            time.sleep(.01)

    try:
        write_json(directory / 'environment.json', environment(args, directory))
        fixture = Path(__file__).with_name('benchmark_fixture.py')
        backend = start('backend', [sys.executable, str(fixture), '--protocol', args.protocol,
                                    '--directory', str(directory), '--fault', args.fault])
        wait(lambda: (directory / 'backend-ready.json').exists())
        backend_port = json.loads((directory / 'backend-ready.json').read_text())['port']
        port = backend_port
        configuration = f'backend=127.0.0.1:{backend_port}\nprotocol={args.protocol}\nhealth_check=off\nmetrics=off\n'
        if mode == 'proxy':
            for attempt in range(3):
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM if args.protocol == 'tcp' else socket.SOCK_DGRAM) as reservation:
                    reservation.bind(('127.0.0.1', 0))
                    port = reservation.getsockname()[1]
                config_path = directory / f'config-{attempt}.conf'
                config_path.write_text(f'listen=127.0.0.1:{port}\n' + configuration)
                name = f'product-{attempt}'
                product = start(name, [str(args.program), '--run', str(config_path)])
                try:
                    wait(lambda: '服务已启动' in (directory / f'{name}.stdout.log').read_text())
                    break
                except RuntimeError:
                    # Retry only an observed bind collision; preserve that process/log record.
                    error_text = (directory / f'{name}.stderr.log').read_text()
                    if 'Address already in use' not in error_text or attempt == 2:
                        raise
                    processes.remove((name, product))
                    product.wait()
                    result['owned_processes'][-1].update(returncode=product.returncode, reaped=True)
            else:
                raise RuntimeError('dynamic port retries exhausted')
        (directory / 'configuration.txt').write_text(configuration + f'entry=127.0.0.1:{port}\nmode={mode}\n')
        for _ in range(args.clients):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM if args.protocol == 'tcp' else socket.SOCK_DGRAM)
            sockets.append(sock)
            sock.settimeout(args.timeout)
            sock.connect(('127.0.0.1', port))
            if args.protocol == 'tcp':
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            else:
                sock.setblocking(False)
        result['client_sources'] = [list(sock.getsockname()) for sock in sockets]
        nonce = uuid.UUID(result['run_id']).bytes

        def phase(number, seconds):
            if args.protocol == 'tcp':
                return tcp_phase(sockets, nonce, number, seconds, args.timeout, args.payload, check)
            return udp_phase(sockets, nonce, number, seconds, args.timeout, args.payload, args.rate, check)

        if args.warmup:
            warmup, _, *_ = phase(0, args.warmup)
            result['warmup_counts_excluded'] = warmup
        if args.fault == 'product-exit':
            if product is None:
                raise RuntimeError('product-exit fault requires proxy')
            product.send_signal(signal.SIGUSR1)
            product.wait(timeout=2)
            check()
        pids = {'generator': os.getpid(), 'backend': backend.pid}
        if product:
            pids['product'] = product.pid
        if args.fault == 'resource':
            pids['backend'] = 2147483647
        sampler = Resources(pids, check)
        sampler.start()
        values, samples, start_ns, end_ns, finish_ns = phase(1, args.duration)
        result['resources'] = sampler.stop()
        if args.fault == 'resource-evidence':
            (directory / 'resource-samples.json').mkdir()
        write_json(directory / 'resource-samples.json', sampler.rows)
        result['counts'] = values
        result['timing'] = {'t0_ns': start_ns, 't1_ns': end_ns, 'drain_end_ns': finish_ns,
                            'send_window_seconds': (end_ns - start_ns) / 1e9,
                            'goodput_elapsed_seconds': (finish_ns - start_ns) / 1e9}
        result['rates'] = rates(values['sent'], values['received'], args.payload,
                                (end_ns - start_ns) / 1e9, (finish_ns - start_ns) / 1e9)
        result['sampled_rtt'] = {**percentiles([row['rtt_ns'] for row in samples]),
                                 'stride': STRIDE, 'coverage': len(samples) / values['received'] if values['received'] else None,
                                 'p99_insufficient_samples': len(samples) < 100}
        result['under_target'] = args.protocol == 'udp' and values['sent'] < values['target']
        result['units'] = {'payload': 'bytes; verified echo only, not request+echo',
                           'rtt': 'monotonic nanoseconds; successful sampled records only',
                           'cpu': 'one logical core = 100 percent'}
        with (directory / 'rtt-samples.jsonl').open('w') as stream:
            for row in samples:
                stream.write(json.dumps(row) + '\n')
        if not values['sent'] or not values['received']:
            raise RuntimeError('no successful transmission')
        check()
        # Exercise the actual final evidence write after processes and traffic existed.
        if args.fault == 'evidence':
            (directory / 'result.json').mkdir()
        if args.fault == 'permission':
            directory.chmod(0o500)
        write_json(directory / 'result.json', result)
        result['valid'] = True
    except BaseException as error:
        result['primary_error'] = f'{type(error).__name__}: {error}'
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, prior_alarm)
        if args.fault == 'permission':
            directory.chmod(0o700)
        if sampler:
            sampler.done.set()
            if sampler.thread:
                sampler.thread.join(2)
                if sampler.thread.is_alive():
                    result['cleanup_errors'].append('sampler thread not joined')
        for sock in sockets:
            sock.close()
        for name, process in reversed(processes):
            try:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
                record = next(r for r in result['owned_processes'] if r['pid'] == process.pid)
                record.update(returncode=process.returncode, reaped=process.poll() is not None)
                if name == 'backend' and process.returncode != 0:
                    result['cleanup_errors'].append(f'backend exit {process.returncode}')
                if args.fault == 'cleanup' and name == 'backend':
                    raise OSError('injected cleanup verification failure after owned PID reap')
            except Exception as error:
                result['cleanup_errors'].append(f'{name}: {error}')
        for stream in logs:
            stream.close()
        if sampler:
            try:
                write_json(directory / 'resource-samples.json', sampler.rows)
            except Exception as error:
                result['primary_error'] = result['primary_error'] or f'resource evidence write: {error}'
        result['fixture_fd'] = {'before': baseline_fd, 'after': len(os.listdir('/proc/self/fd'))}
        if result['fixture_fd']['before'] != result['fixture_fd']['after']:
            result['cleanup_errors'].append('generator fixture fd leak')
        backend_cleanup = directory / 'backend-cleanup.json'
        if backend_cleanup.exists():
            try:
                result['backend_cleanup'] = json.loads(backend_cleanup.read_text())
                if result['backend_cleanup']['fd_before'] != result['backend_cleanup']['fd_after']:
                    result['cleanup_errors'].append('backend fixture fd leak')
            except Exception as error:
                result['cleanup_errors'].append(f'backend cleanup evidence: {error}')
        elif any(name == 'backend' for name, _ in processes):
            result['cleanup_errors'].append('missing backend cleanup evidence')
        if result['primary_error'] or result['cleanup_errors']:
            result['valid'] = False
        result['utc_end'] = utc()
        result['exit_code'] = 0 if result['valid'] else 1
        try:
            write_json(directory / 'result.json', result)
        except Exception as error:
            result['valid'] = False
            result['exit_code'] = 1
            result['primary_error'] = result['primary_error'] or f'evidence write failed: {error}'
            try:
                write_json(directory / 'failure.json', result)
            except Exception as secondary:
                # A removed/unwritable output must not prevent owned process cleanup.
                result['cleanup_errors'].append(f'fallback evidence write: {secondary}')
                print(json.dumps(result), file=sys.stderr)
        signal.signal(signal.SIGTERM, prior_term)
    return result


def main(argv=None):
    args = arguments(argv)
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for index in range(args.repeats):
        modes = ['direct', 'proxy'] if args.mode == 'paired' else [args.mode]
        if index % 2:
            modes.reverse()
        for mode in modes:
            result = run_one(args, mode, index, modes)
            results.append(result)
            if not result['valid']:
                break
        if not results[-1]['valid']:
            break
    summary = {'schema_version': 1, 'valid': all(r['valid'] for r in results),
               'runs': [{'path': f"{r['repeat']:02d}-{r['mode']}", 'valid': r['valid'],
                         'primary_error': r['primary_error'], 'cleanup_errors': r['cleanup_errors']} for r in results],
               'aggregate': aggregate(results)}
    write_json(args.output / 'summary.json', summary)
    print(json.dumps({'output': str(args.output), 'valid': summary['valid'], 'runs': len(results)}))
    return 0 if summary['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
