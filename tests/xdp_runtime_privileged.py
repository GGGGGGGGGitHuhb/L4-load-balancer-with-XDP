#!/usr/bin/env python3
"""S3 runtime control acceptance in disposable anonymous network namespaces."""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess as S
import sys
import time
from xdp_udp_dsr_privileged import Topology, Kernel, command, VIP, PORT, frame, reference_hash

ECHO = r'''
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind((sys.argv[1],39009))
print('PROBE_READY',flush=True)
while True:
 data,peer=s.recvfrom(4096);s.sendto(data,peer)
'''

STREAM = r'''
import json,socket,sys,time,os
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(.8);s.bind(('198.18.0.1',45000))
count=0;failures=0
while not os.path.exists(sys.argv[1]):
 s.sendto(b'dsr-stream',('198.19.0.100',39001))
 try:
  data,peer=s.recvfrom(4096)
  assert peer==('198.19.0.100',39001) and data in [b'0:dsr-stream',b'1:dsr-stream'],(data,peer)
  count+=1
 except TimeoutError:failures+=1
 time.sleep(.002)
assert count>100 and failures==0,(count,failures)
print(json.dumps({'received':count,'timeouts':failures}))
'''


class RuntimeTopology(Topology):
    def __init__(self, args):
        super().__init__(args)
        self.config = self.directory/'runtime.conf'
        self.fault = self.directory/'fault.txt'
        self.fault.write_text('')
        self.probes = []
        for index in range(2):
            self.ip(None, 'addr', 'add', f'198.20.{index}.1/24', 'dev', f'e{index}')
            self.ip(None, 'neigh', 'replace', f'198.20.{index}.2', 'lladdr', self.mac(f'be{index}', f'b{index}'), 'nud', 'permanent', 'dev', f'e{index}')
            log = self.directory/f'probe-{index}.log'
            with log.open('w') as output:
                process = S.Popen([*self.prefix(f'be{index}'), sys.executable, '-u', '-c', ECHO, f'198.20.{index}.2'], stdout=output, stderr=output)
            self.processes.append(process); self.probes.append(process)
            self.wait(lambda: 'PROBE_READY' in log.read_text(), 'probe server startup')
        self.configure([0, 1])

    def wait(self, predicate, reason, timeout=12):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.04)
        raise AssertionError('timeout: '+reason)

    def configure(self, order):
        text = 'schema=1\n'
        for index in order:
            text += f'target=backend{index} e{index}@{self.mac(f"be{index}", f"b{index}")} 198.20.{index}.2:39009\n'
        temporary = self.config.with_suffix('.new')
        temporary.write_text(text)
        temporary.replace(self.config)

    def invocation(self, mode, device='xb'):
        return [str(self.args.loader), 'attach', '--dev', device, '--object', str(self.args.object),
                '--udp-dsr-runtime', '--vip', f'{VIP}:{PORT}', '--runtime-config', str(self.config), '--mode', mode]

    def start_runtime(self, mode, pipe=False, closed=False):
        log = self.directory/f'runtime-{len(self.processes)}.log'
        invocation = self.invocation(mode)
        if closed:
            invocation = [sys.executable, '-c', 'import os,sys;os.close(0);os.execv(sys.argv[1],sys.argv[1:])', *invocation]
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(self.args.fault_library), L4LB_RUNTIME_FAULT_FILE=str(self.fault))
        with log.open('w') as output:
            process = S.Popen(invocation, stdout=S.PIPE if pipe else output, stderr=output, env=env)
        self.processes.append(process); self.logs[process.pid] = log
        deadline = time.monotonic()+8
        while time.monotonic() < deadline:
            if pipe:
                text = process.stdout.readline().decode() if select.select([process.stdout], [], [], .02)[0] else ''
            else:
                text = log.read_text()
            match = re.search(r'^READY .*prog_id=(\d+).*schema=3.*generation=1.*active_backends=0', text, re.M)
            if match:
                return process, int(match[1])
            if process.poll() is not None:
                raise AssertionError(log.read_text())
            time.sleep(.02)
        raise AssertionError('runtime READY timeout '+log.read_text())

    def active(self, process):
        # An ID may retire between lookup and get_fd; retry from the owned outer.
        for _ in range(30):
            try:
                return self.read_active_once(process)
            except FileNotFoundError:
                time.sleep(.001)
        raise AssertionError('active map never stabilized')

    def read_active_once(self, process):
        kernel = Kernel()
        outer = None
        for path in Path(f'/proc/{process.pid}/fdinfo').iterdir():
            match = re.search(r'^map_id:\s+(\d+)$', path.read_text(), re.M)
            if not match:
                continue
            fd = kernel.checked('bpf_map_get_fd_by_id', int(match[1]))
            try:
                if kernel.info(fd)[24:40].split(b'\0')[0] == b'l4lb_active_v3':
                    outer = struct.unpack('=I', kernel.lookup(fd, size=4))[0]
                    break
            finally:
                os.close(fd)
        assert outer is not None
        fd = kernel.checked('bpf_map_get_fd_by_id', outer)
        try:
            data = kernel.lookup(fd, size=1048)
            version, count, generation = struct.unpack_from('=IIQ', data)
            assert version == 3 and count <= 64
            key, value = C.c_uint32(0), C.create_string_buffer(data)
            assert kernel.lib.bpf_map_update_elem(fd, C.byref(key), value, 0) < 0
            return {'id': outer, 'count': count, 'generation': generation,
                    'macs': [data[28+16*i:34+16*i].hex() for i in range(count)]}
        finally:
            os.close(fd)

    def wait_active(self, process, order):
        wanted = [self.mac(f'be{i}', f'b{i}').replace(':', '') for i in order]
        self.wait(lambda: process.poll() is None and self.active(process)['macs'] == wanted, f'active {order}')
        return self.active(process)

    def traffic(self, order):
        for port in range(40000, 40008):
            index = order[reference_hash(frame(port)) % len(order)]
            answers = self.client(port)
            assert answers == [[f'{index}:dsr-roundtrip', [VIP, PORT]]] * 2, (order, port, answers)

    def hup(self, process):
        process.send_signal(signal.SIGHUP)

    def concurrent_publications(self, process):
        stop = self.directory/'stream-stop'
        output = self.directory/'stream.json'
        with output.open('w') as log:
            reader = S.Popen([*self.prefix('client'), sys.executable, '-u', '-c', STREAM, str(stop)], stdout=log, stderr=log)
        self.processes.append(reader)
        for iteration in range(100):
            previous = self.active(process)['generation']
            order = [0, 1] if iteration % 2 == 0 else [1, 0]
            self.configure(order); self.hup(process)
            self.wait(lambda: self.active(process)['generation'] > previous, '100 live publications')
            self.wait_active(process, order)
            assert reader.poll() is None, output.read_text()
        stop.write_text('stop')
        assert reader.wait(timeout=4) == 0, output.read_text()
        for index in range(2):
            expected = (self.mac(f'be{index}', f'b{index}') + self.mac(None, f'e{index}')).replace(':', '')
            rows = [json.loads(line) for line in (self.directory/f'capture-{index}.jsonl').read_text().splitlines()]
            matched = [row for row in rows if bytes.fromhex(row['ip'])[16:20] == bytes([198,19,0,100])]
            assert matched and all(row['mac'] == expected for row in matched)
        return json.loads(output.read_text())

    def run_runtime(self, mode):
        checks = []
        if mode == 'native':
            for i in range(2):
                self.ip(f'be{i}', 'link', 'set', f'b{i}', 'xdpdrv', 'obj', self.args.pass_object, 'sec', 'xdp')
        process, ident = self.start_runtime(mode)
        assert self.active(process)['count'] == 0
        assert self.client(40100, timeout=.15) == [None, None]
        self.wait_active(process, [0, 1]); self.traffic([0, 1])
        checks += ['startup_unknown_drop', 'two_backend_actual_echo_health_and_roundtrip']
        previous = self.active(process)
        self.hup(process)
        self.wait(lambda: 'status=unchanged' in self.logs[process.pid].read_text(), 'unchanged reload')
        assert self.active(process) == previous
        self.config.write_text('schema=7\n')
        self.hup(process)
        self.wait(lambda: 'XDP_RELOAD status=rejected' in self.logs[process.pid].read_text(), 'bad config rejected')
        assert self.active(process) == previous; self.traffic([0, 1])
        self.configure([0, 1])
        original_text = self.config.read_text()
        self.config.write_text(original_text.replace('198.20.0.2:39009', VIP+':39009'))
        rejected_before = self.logs[process.pid].read_text().count('XDP_RELOAD status=rejected')
        self.hup(process)
        self.wait(lambda: self.logs[process.pid].read_text().count('XDP_RELOAD status=rejected') > rejected_before, 'shared VIP probe rejected')
        assert self.active(process) == previous; self.traffic([0, 1])
        checks.append('shared_vip_probe_rejected')
        self.fault.write_text('outer-update')
        self.configure([0]); self.hup(process)
        self.wait(lambda: 'XDP_PUBLISH status=failed' in self.logs[process.pid].read_text(), 'precommit publish rejected')
        assert self.active(process) == previous; self.traffic([0, 1])
        self.fault.write_text('')
        self.hup(process); self.wait_active(process, [0]); self.traffic([0])
        self.configure([0, 1]); self.hup(process)
        self.wait_active(process, [0, 1]); self.traffic([0, 1])
        self.configure([1, 0]); self.hup(process)
        self.wait_active(process, [1, 0]); self.traffic([1, 0])
        if mode == 'generic':
            stream = self.concurrent_publications(process)
            checks.append({'100_live_publications_complete_mac_and_reply': stream})
        checks += ['unchanged_retains_generation', 'bad_file_keeps_old_flow', 'precommit_fault_keeps_old_flow', 'delete_add_reorder_live']
        self.fault.write_text('outer-update')
        old_applied = self.active(process)
        self.probes[0].send_signal(signal.SIGSTOP)
        self.wait(lambda: 'XDP_PUBLISH status=failed reason=health' in self.logs[process.pid].read_text(), 'health publish failure')
        baseline = self.logs[process.pid].read_text().count('XDP_PUBLISH status=failed reason=health')
        time.sleep(2.2)
        attempts = self.logs[process.pid].read_text().count('XDP_PUBLISH status=failed reason=health') - baseline
        assert 1 <= attempts <= 3, attempts
        assert self.active(process) == old_applied
        self.traffic([1, 0])
        self.fault.write_text('')
        self.wait_active(process, [1]); self.traffic([1])
        self.probes[0].send_signal(signal.SIGCONT)
        self.wait_active(process, [1, 0]); self.traffic([1, 0])
        for probe in self.probes:
            probe.send_signal(signal.SIGSTOP)
        self.wait_active(process, [])
        assert self.client(40000, timeout=.15) == [None, None]
        for probe in self.probes:
            probe.send_signal(signal.SIGCONT)
        self.wait_active(process, [1, 0]); self.traffic([1, 0])
        checks += ['health_publish_failure_keeps_applied_and_bounded_retry', 'single_probe_failure_evict_recover', 'all_unhealthy_drop_recover']
        command(*self.invocation(mode), expected=1)
        self.detach(mode, 4294967295, expected=1)
        assert self.attached() == ident
        storm = S.Popen([sys.executable, '-c', 'import os,signal,sys,time;end=time.monotonic()+3\nwhile time.monotonic()<end:\n try:os.kill(int(sys.argv[1]),signal.SIGHUP)\n except ProcessLookupError:break\n time.sleep(.001)', str(process.pid)])
        self.processes.append(storm)
        time.sleep(.2)
        assert storm.poll() is None
        started = time.monotonic()
        self.stop(process)
        assert time.monotonic()-started < 3 and self.attached() == 0
        checks += ['duplicate_wrong_id_preserved', 'hup_storm_term_priority']
        # A committed outer update followed by a read fault must terminate, never reject/revert.
        process, ident = self.start_runtime(mode)
        self.wait_active(process, [1, 0])
        self.fault.write_text('post-read')
        self.configure([0]); self.hup(process)
        assert process.wait(timeout=8) == 1
        assert self.attached() == 0
        self.fault.write_text('')
        checks += ['postcommit_failure_terminates_and_detaches']
        self.configure([])
        process, ident = self.start_runtime(mode, pipe=True)
        process.stdout.close()
        assert process.wait(timeout=5) == 1 and self.attached() == 0
        checks += ['empty_config_closed_stdout_terminates']
        process, ident = self.start_runtime(mode, closed=True)
        self.detach(mode, ident)
        replacement, replacement_id = self.start_runtime(mode)
        self.stop(process, expected=1)
        assert self.attached() == replacement_id
        self.stop(replacement)
        process, ident = self.start_runtime(mode)
        self.stop(process, signal.SIGINT)
        assert self.attached() == 0
        checks.append('sigint_cleanup')
        process, ident = self.start_runtime(mode)
        process.kill(); assert process.wait(timeout=5) == -signal.SIGKILL
        assert self.attached() == ident
        self.detach(mode, ident)
        checks += ['closed_stdin_replacement_protection', 'sigkill_explicit_recovery']
        self.configure([0, 1])
        process, ident = self.start_runtime(mode)
        self.wait_active(process, [0, 1])
        self.ip(None, 'link', 'set', 'e0', 'down')
        self.wait_active(process, [1]); self.traffic([1])
        self.ip(None, 'link', 'set', 'e0', 'up')
        self.wait_active(process, [0, 1]); self.traffic([0, 1])
        self.ip(None, 'link', 'delete', 'e0')
        self.wait_active(process, [1]); self.traffic([1])
        self.stop(process); assert self.attached() == 0
        self.configure([1])
        process, ident = self.start_runtime(mode)
        self.ip(None, 'link', 'delete', 'xb')
        self.stop(process)
        checks += ['egress_down_delete_health_conservative_cleanup', 'ingress_removed_cleanup']
        return checks

    def close(self):
        for probe in getattr(self, 'probes', []):
            if probe.poll() is None:
                probe.send_signal(signal.SIGCONT)
        super().close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--pass-object', type=Path, required=True)
    parser.add_argument('--fault-library', type=Path, required=True)
    parser.add_argument('--log-dir', type=Path, required=True)
    parser.add_argument('--namespace-timeout', type=float, default=260, help='Bound each isolated mode, including failure cleanup')
    parser.add_argument('--inside', choices=['generic', 'native'], help=argparse.SUPPRESS)
    args = parser.parse_args()
    for name in ['loader', 'object', 'pass_object', 'fault_library']:
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.log_dir = args.log_dir.resolve()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    assert os.geteuid() == 0, 'requires root; runner creates its own anonymous netns'
    if not args.inside:
        before = command('ip', '-j', 'link'), command('ip', '-j', 'route')
        results = {}
        for mode in ['generic', 'native']:
            log = args.log_dir/f'{mode}-summary.json'
            with log.open('w') as output:
                process = S.Popen(['unshare', '--net', sys.executable, str(Path(__file__).resolve()),
                                   *sys.argv[1:], '--inside', mode], stdout=output, stderr=output,
                                  start_new_session=True)
                timed_out = False
                try:
                    result = process.wait(timeout=args.namespace_timeout)
                except S.TimeoutExpired:
                    timed_out = True
                    process.terminate()
                    try:
                        process.wait(timeout=12)
                    except S.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                    # Also reap any surviving descendants if cleanup itself failed.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    result = process.returncode
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
            assert not timed_out, f'{mode} timeout; isolated process group {process.pid} cleaned; preserved {log}'
            assert result == 0, f'{mode} failed; preserved {log}'
            results[mode] = json.loads(log.read_text())
        assert before == (command('ip', '-j', 'link'), command('ip', '-j', 'route'))
        print(json.dumps({'modes': results, 'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [args.loader, args.object, args.pass_object, args.fault_library, Path(__file__)]}}, indent=2))
        return
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
    args.log_dir /= args.inside
    args.log_dir.mkdir(parents=True, exist_ok=True)
    def request_stop(signum, frame):
        raise RuntimeError(f'runtime test interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    topology = RuntimeTopology.__new__(RuntimeTopology)
    topology.processes = []
    try:
        topology.__init__(args)
        print(json.dumps(topology.run_runtime(args.inside), indent=2))
    finally:
        topology.close()


if __name__ == '__main__':
    main()
