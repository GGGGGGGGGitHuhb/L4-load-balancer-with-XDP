#!/usr/bin/env python3
"""Independent runtime stdout lifecycle checks in disposable network namespaces."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess as S
import sys
import time

from xdp_runtime_privileged import RuntimeTopology, command


class OutputTopology(RuntimeTopology):
    def start_output(self, mode, label, full_pipe=False, inject=False):
        log = self.directory / f'{label}.log'
        env = os.environ.copy()
        env.pop('LD_PRELOAD', None)
        self.output_fault = self.directory / f'{label}-fault.txt'
        self.output_trace = self.directory / f'{label}-trace.txt'
        self.output_fault.write_text('')
        if inject:
            env.update(LD_PRELOAD=str(self.args.output_library),
                       L4LB_OUTPUT_FAULT_FILE=str(self.output_fault),
                       L4LB_OUTPUT_FAULT_TRACE=str(self.output_trace))
        read_fd = write_fd = None
        capacity = 0
        try:
            if full_pipe:
                read_fd, write_fd = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
                capacity = fcntl.fcntl(write_fd, fcntl.F_GETPIPE_SZ)
                filled = 0
                while True:
                    try:
                        filled += os.write(write_fd, b'X' * 4096)
                    except BlockingIOError:
                        break
                assert filled == capacity, (filled, capacity)
            with log.open('w') as output:
                process = S.Popen(self.invocation(mode), stdout=write_fd if full_pipe else output,
                                  stderr=output, env=env)
            self.processes.append(process)
            self.logs[process.pid] = log
            if write_fd is not None:
                os.close(write_fd)
                write_fd = None
            self.wait_for_active(process, [0, 1])
            return process, read_fd, capacity
        except BaseException:
            if read_fd is not None:
                os.close(read_fd)
            raise
        finally:
            if write_fd is not None:
                os.close(write_fd)

    def wait_for_active(self, process, order):
        wanted = [self.mac(f'be{i}', f'b{i}').replace(':', '') for i in order]
        def matches():
            assert process.poll() is None, self.logs[process.pid].read_text()
            try:
                return self.active(process)['macs'] == wanted
            except (FileNotFoundError, AssertionError):
                return False
        self.wait(matches, f'output test active {order}')
        return self.active(process)

    def actual_pipe_backpressure(self, mode):
        process, read_fd, capacity = self.start_output(mode, 'real-full-pipe', full_pipe=True)
        try:
            self.traffic([0, 1])
            self.probes[0].send_signal(signal.SIGSTOP)
            self.wait_for_active(process, [1])
            self.traffic([1])
            self.probes[0].send_signal(signal.SIGCONT)
            self.wait_for_active(process, [0, 1])
            started = time.monotonic()
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=4) == 1, self.logs[process.pid].read_text()
            elapsed = time.monotonic() - started
            assert elapsed < 3 and self.attached() == 0, elapsed
            # Nothing was read while the loader was running: this is real pipe
            # backpressure, not an injected EAGAIN or a closed output reader.
            contents = os.read(read_fd, capacity + 4096)
            assert contents == b'X' * capacity, len(contents)
            assert '背压超时' in self.logs[process.pid].read_text()
            return {'case': 'actual_full_pipe_health_and_bounded_stop',
                    'pipe_capacity': capacity, 'stop_seconds': elapsed}
        finally:
            self.probes[0].send_signal(signal.SIGCONT)
            os.close(read_fd)

    def injected_queue_overflow(self, mode):
        process, _, _ = self.start_output(mode, 'injected-overflow', inject=True)
        self.output_fault.write_text('eagain')
        deadline = time.monotonic() + 35
        signals = 0
        while process.poll() is None and time.monotonic() < deadline:
            process.send_signal(signal.SIGHUP)
            signals += 1
            time.sleep(.003)
        assert process.poll() is not None, '64KiB queue did not terminate'
        assert process.wait(timeout=2) == 1 and self.attached() == 0
        text = self.logs[process.pid].read_text()
        assert '输出队列超过64KiB' in text, text
        largest = int(self.output_trace.read_text())
        assert 65000 <= largest <= 65536, largest
        return {'case': 'injected_eagain_queue_overflow', 'largest_pending_bytes': largest,
                'hup_signals_sent': signals}

    def unchanged_write_failure(self, mode):
        process, _, _ = self.start_output(mode, 'unchanged-eio', inject=True)
        before = self.active(process)
        self.output_fault.write_text('unchanged-eio')
        process.send_signal(signal.SIGHUP)
        assert process.wait(timeout=4) == 1, self.logs[process.pid].read_text()
        assert self.attached() == 0
        assert self.output_trace.read_text() == 'unchanged_eio_once\n'
        text = self.logs[process.pid].read_text()
        assert 'runtime 输出失败' in text and 'XDP_RELOAD status=rejected' not in text, text
        return {'case': 'unchanged_one_shot_stdout_eio_fatal', 'generation_before': before['generation']}

    def verify(self, mode):
        if mode == 'native':
            for index in range(2):
                self.ip(f'be{index}', 'link', 'set', f'b{index}', 'xdpdrv',
                        'obj', str(self.args.pass_object), 'sec', 'xdp')
        return [self.actual_pipe_backpressure(mode), self.injected_queue_overflow(mode),
                self.unchanged_write_failure(mode)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ['loader', 'object', 'pass-object', 'output-library', 'log-dir']:
        parser.add_argument('--' + argument, type=Path, required=True)
    parser.add_argument('--inside', choices=['generic', 'native'], help=argparse.SUPPRESS)
    args = parser.parse_args()
    for name in ['loader', 'object', 'pass_object', 'output_library']:
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.log_dir = args.log_dir.resolve()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    assert os.geteuid() == 0, 'root required only for disposable netns and BPF'
    if not args.inside:
        before = command('ip', '-j', 'link'), command('ip', '-j', 'route')
        results = {}
        for mode in ['generic', 'native']:
            log = args.log_dir / f'{mode}-summary.json'
            with log.open('w') as output:
                process = S.Popen(['unshare', '--net', sys.executable, str(Path(__file__).resolve()),
                                   *sys.argv[1:], '--inside', mode], stdout=output, stderr=output,
                                  start_new_session=True)
                try:
                    result = process.wait(timeout=85)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=15)
                        except S.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
            assert result == 0, f'{mode} failed; preserved {log}'
            results[mode] = json.loads(log.read_text())
        assert before == (command('ip', '-j', 'link'), command('ip', '-j', 'route'))
        paths = [args.loader, args.object, args.output_library, Path(__file__),
                 Path(__file__).with_name('RuntimeOutputFaults.c')]
        print(json.dumps({'modes': results, 'host_network_unchanged': True,
                          'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}, indent=2))
        return
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
    args.log_dir /= args.inside
    args.log_dir.mkdir(parents=True, exist_ok=True)
    def request_stop(signum, frame):
        raise RuntimeError(f'output test interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    topology = OutputTopology.__new__(OutputTopology)
    try:
        topology.__init__(args)
        print(json.dumps(topology.verify(args.inside), indent=2))
    finally:
        topology.close()


if __name__ == '__main__':
    main()
