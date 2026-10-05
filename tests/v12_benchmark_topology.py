"""Disposable namespace lifecycle; all addresses/routes belong to this run."""
import ctypes as C
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess as S
import sys
import threading
import time

from v12_benchmark_data import sha
from v12_benchmark_measure import atomic_json, pid_sample, system_sample

VIP = '198.19.0.100'
PORT = 39001
SCRIPT = str(Path(__file__).with_name('v12_benchmark.py').resolve())


def command(argv):
    result = S.run(list(map(str, argv)), capture_output=True, text=True, timeout=4)
    if result.returncode:
        raise RuntimeError(f'command failed {argv}: {result.stderr[-1800:]}')
    return result.stdout


def active(pid, library):
    lib = C.CDLL(library, use_errno=True)
    for path in Path(f'/proc/{pid}/fdinfo').iterdir():
        match = re.search(r'^map_id:\s+(\d+)$', path.read_text(), re.M)
        if not match:
            continue
        fd = lib.bpf_map_get_fd_by_id(int(match[1]))
        if fd < 0:
            continue
        try:
            info, length = C.create_string_buffer(256), C.c_uint32(256)
            if lib.bpf_obj_get_info_by_fd(fd, info, C.byref(length)):
                raise RuntimeError('map info failed')
            if info.raw[24:40].split(b'\0')[0] != b'l4lb_active_v3':
                continue
            key, value = C.c_uint32(0), C.c_uint32()
            if lib.bpf_map_lookup_elem(fd, C.byref(key), C.byref(value)):
                raise RuntimeError('outer lookup failed')
            inner = lib.bpf_map_get_fd_by_id(value.value)
            if inner < 0:
                return active(pid, library)
            try:
                data = C.create_string_buffer(1048)
                if lib.bpf_map_lookup_elem(inner, C.byref(key), data):
                    raise RuntimeError('inner lookup failed')
                version, count, generation = struct.unpack_from('=IIQ', data.raw)
                if version != 3:
                    raise RuntimeError('active schema')
                return dict(active=count, generation=generation,
                            macs=[data.raw[28 + 16 * i:34 + 16 * i].hex() for i in range(count)])
            finally:
                os.close(inner)
        finally:
            os.close(fd)
    raise RuntimeError('active map missing')


class Topology:
    def __init__(self, directory, identity, spec, fault):
        self.directory, self.identity, self.spec, self.fault = Path(directory), identity, spec, fault
        self.owned, self.namespaces, self.logs, self.events = [], {}, [], []
        self.errors, self.threads, self.commands = [], [], []
        self.pids, self.product = {}, None
        self.resources, self.sampling_errors = [], []
        self.stop_sampling = threading.Event()
        self.started = time.monotonic()

    def spawn(self, argv, name=None, pipe=False):
        self.commands.append(list(map(str, argv)))
        log = self.directory / f'process-{len(self.owned)}.log'
        handle = log.open('w')
        self.logs.append(handle)
        process = S.Popen(list(map(str, argv)), stdout=S.PIPE if pipe else handle, stderr=handle)
        self.owned.append(process)
        if name:
            self.pids[name] = process.pid
        if pipe:
            def consume():
                try:
                    for line in process.stdout:
                        if sum(len(e.get('line', '')) for e in self.events) > 500000:
                            raise RuntimeError('output evidence limit')
                        self.events.append(dict(type='output', ns=time.monotonic_ns(), line=line.decode().strip()))
                except Exception as exc:
                    self.sampling_errors.append(str(exc))
            thread = threading.Thread(target=consume, daemon=True)
            thread.start()
            self.threads.append(thread)
        return process

    def prefix(self, namespace):
        return [] if namespace is None else ['nsenter', '-t', self.namespaces[namespace], '-n']

    def call(self, namespace, *argv):
        return command([*self.prefix(namespace), *argv])

    def ip(self, namespace, *argv):
        return self.call(namespace, 'ip', *argv)

    def mac(self, namespace, device):
        return json.loads(self.ip(namespace, '-j', 'link', 'show', device))[0]['address']

    def wait(self, predicate, reason, timeout=10):
        deadline = time.monotonic() + timeout
        while not predicate():
            self.check()
            if time.monotonic() >= deadline:
                raise RuntimeError('startup timeout: ' + reason)
            time.sleep(.01)

    def check(self):
        if self.sampling_errors:
            raise RuntimeError('background sampler/output failure: ' + '; '.join(self.sampling_errors))
        for process in self.owned:
            if process.poll() is not None:
                raise RuntimeError(f'ready child exited pid={process.pid} code={process.returncode}')

    def pair(self, local, remote, namespace, local_namespace=None):
        self.ip(None, 'link', 'add', local, 'type', 'veth', 'peer', 'name', remote)
        self.ip(None, 'link', 'set', remote, 'netns', self.namespaces[namespace])
        if local_namespace:
            self.ip(None, 'link', 'set', local, 'netns', self.namespaces[local_namespace])
        self.ip(local_namespace, 'link', 'set', local, 'mtu', '1500', 'up')
        self.ip(namespace, 'link', 'set', remote, 'mtu', '1500', 'up')

    def configure(self, order):
        content = 'schema=1\n' + ''.join(f'target=backend{i} e{i}@{self.mac(f"be{i}", f"b{i}")} 198.20.{i}.2:39009\n' for i in order)
        path = self.directory / 'runtime.conf'
        temp = path.with_suffix('.new')
        temp.write_text(content)
        temp.replace(path)
        self.events.append(dict(type='config', ns=time.monotonic_ns(), content=content, sha256=sha(path), order=order))
        return sha(path)

    def setup(self):
        self.ip(None, 'link', 'set', 'lo', 'up')
        for name in ('client', 'be0', 'be1'):
            process = self.spawn(['unshare', '--net', 'sleep', '3600'])
            self.wait(lambda: os.readlink(f'/proc/{process.pid}/ns/net') != os.readlink('/proc/self/ns/net'), 'netns')
            self.namespaces[name] = process.pid
            self.ip(name, 'link', 'set', 'lo', 'up')
        self.pair('xb', 'xa', 'client')
        self.ip(None, 'addr', 'add', '198.18.0.2/24', 'dev', 'xb')
        self.ip('client', 'addr', 'add', '198.18.0.1/24', 'dev', 'xa')
        self.ip('client', 'neigh', 'replace', '198.18.0.2', 'lladdr', self.mac(None, 'xb'), 'nud', 'permanent', 'dev', 'xa')
        for i in range(2):
            name = f'be{i}'
            self.pair(f'e{i}', f'b{i}', name)
            self.ip(None, 'addr', 'add', f'198.20.{i}.1/24', 'dev', f'e{i}')
            self.ip(name, 'addr', 'add', f'198.20.{i}.2/24', 'dev', f'b{i}')
            self.ip(None, 'neigh', 'replace', f'198.20.{i}.2', 'lladdr', self.mac(name, f'b{i}'), 'nud', 'permanent', 'dev', f'e{i}')
            self.ip(name, 'neigh', 'replace', f'198.20.{i}.1', 'lladdr', self.mac(None, f'e{i}'), 'nud', 'permanent', 'dev', f'b{i}')
            self.pair(f'q{i}', f'r{i}', name, 'client')
            self.ip('client', 'addr', 'add', f'198.18.{i+1}.1/24', 'dev', f'q{i}')
            self.ip(name, 'addr', 'add', f'198.18.{i+1}.2/24', 'dev', f'r{i}')
            self.ip(name, 'addr', 'add', VIP + '/32', 'dev', 'lo')
            self.ip(name, 'route', 'add', '198.18.0.1/32', 'via', f'198.18.{i+1}.1')
            self.ip(name, 'neigh', 'replace', f'198.18.{i+1}.1', 'lladdr', self.mac('client', f'q{i}'), 'nud', 'permanent', 'dev', f'r{i}')
        gateway = '198.18.1.2' if self.spec['path'] == 'direct' else '198.18.0.2'
        self.ip('client', 'route', 'add', VIP + '/32', 'via', gateway)
        if self.spec['path'] == 'direct':
            self.ip('client', 'neigh', 'replace', gateway, 'lladdr', self.mac('be0', 'r0'), 'nud', 'permanent', 'dev', 'q0')
        for name in (None, 'client', 'be0', 'be1'):
            self.call(name, sys.executable, '-c', "from pathlib import Path;[p.write_text('0') for p in Path('/proc/sys/net/ipv4/conf').glob('*/rp_filter')]")
        for name, device in [('client', 'xa'), ('client', 'q0'), ('client', 'q1'), ('be0', 'b0'), ('be1', 'b1'), ('be0', 'r0'), ('be1', 'r1'), (None, 'e0'), (None, 'e1')]:
            self.call(name, 'ethtool', '-K', device, 'tx', 'off')
        for i in range(2):
            self.spawn([*self.prefix(f'be{i}'), sys.executable, SCRIPT, '_echo', str(self.directory), str(i), self.fault], f'backend{i}')
            self.wait(lambda: (self.directory / f'echo-{i}.ready').exists(), 'echo ready')
        products = self.identity['binaries']
        path = self.spec['path']
        if path == 'proxy':
            self.ip(None, 'addr', 'add', VIP + '/32', 'dev', 'lo')
            config = self.directory / 'proxy.conf'
            config.write_text(f'protocol=udp\nlisten={VIP}:{PORT}\nbackend=198.20.0.2:{PORT}\nbackend=198.20.1.2:{PORT}\nscheduler=round_robin\nhealth_check=off\nmetrics=off\n')
            self.product = self.spawn([products['proxy']['path'], '--run', config], 'product', True)
            self.wait(lambda: any('服务已启动' in e.get('line', '') for e in self.events), 'proxy ready')
        elif path != 'direct':
            mode = path.split('-')[1]
            if mode == 'native':
                for i in range(2):
                    self.ip(f'be{i}', 'link', 'set', f'b{i}', 'xdpdrv', 'obj', products['pass']['path'], 'sec', 'xdp')
            runtime = path.startswith('runtime')
            args = [products['loader']['path'], 'attach', '--dev', 'xb', '--mode', mode,
                    '--object', products['runtime' if runtime else 'static']['path'], '--vip', f'{VIP}:{PORT}']
            if runtime:
                self.configure([0, 1])
                args += ['--udp-dsr-runtime', '--runtime-config', str(self.directory / 'runtime.conf')]
            else:
                args += ['--udp-dsr']
                for i in range(2):
                    args += ['--target', f'e{i}@{self.mac(f"be{i}", f"b{i}")}']
            self.product = self.spawn(args, 'product', True)
            self.wait(lambda: any(e.get('line', '').startswith('READY ') for e in self.events), 'loader ready')
            if runtime:
                self.wait(lambda: active(self.product.pid, self.identity['libbpf'])['active'] == 2 and sum('to=Healthy' in e.get('line', '') for e in self.events) == 2, 'runtime healthy two')
        if time.monotonic() - self.started > 15:
            raise RuntimeError('startup preparation exceeded 15s')
        self.network = {str(name): dict(address=json.loads(self.ip(name, '-j', 'addr')), route=json.loads(self.ip(name, '-j', 'route')),
                                       neighbor=json.loads(self.ip(name, '-j', 'neigh'))) for name in (None, 'client', 'be0', 'be1')}
        self.network['offload'] = {f'{name}/{dev}': self.call(name, 'ethtool', '-k', dev) for name, dev in [('client', 'xa'), ('client', 'q0'), ('be0', 'b0'), ('be1', 'b1')]}
        self.network['pass_helper'] = path.endswith('native')

    def sampling(self):
        def sample():
            try:
                while not self.stop_sampling.is_set():
                    if self.fault == 'sampler' and len(self.resources) >= 4:
                        raise OSError('injected sampler read failure after traffic')
                    self.resources.append(dict(ns=time.monotonic_ns(), pids={name: pid_sample(pid) for name, pid in self.pids.items()}, system=system_sample()))
                    self.stop_sampling.wait(.05)
            except Exception as exc:
                self.sampling_errors.append(str(exc))
        self.sampler = threading.Thread(target=sample, daemon=True)
        self.sampler.start()

    def measure(self, nonce):
        preflight = dict(active=None, generation=None, healthy=None)
        if self.spec['path'].startswith('runtime'):
            preflight.update(active(self.product.pid, self.identity['libbpf']))
            preflight['healthy'] = sum('to=Healthy' in e.get('line', '') for e in self.events)
        self.spawn([*self.prefix('client'), sys.executable, SCRIPT, '_client', str(self.directory), json.dumps(self.spec), nonce], 'generator')
        self.wait(lambda: (self.directory / 'client.ready').exists(), 'client warmup', 10)
        preflight.update(json.loads((self.directory / 'preflight.json').read_text()))
        self.sampling()
        self.wait(lambda: self.resources, 'resource barrier')
        start = time.monotonic_ns() + 100_000_000
        end = start + int(self.spec['window_s'] * 1e9)
        atomic_json(self.directory / 'go.json', start)
        dispatched = 0
        while not (self.directory / 'traffic.json').exists() or not self.resources or self.resources[-1]['ns'] < end:
            self.check()
            now = time.monotonic_ns()
            if self.fault == 'child-exit' and now > start + 200_000_000 and (self.directory / 'traffic.started').exists():
                os.kill(self.pids['backend0'], signal.SIGKILL)
            if self.fault == 'watchdog' and now > start + 200_000_000 and (self.directory / 'traffic.started').exists():
                while True:
                    time.sleep(1)
            if self.fault == 'sigterm' and now > start + 200_000_000 and (self.directory / 'traffic.started').exists():
                os.kill(os.getpid(), signal.SIGTERM)
            if self.fault == 'write' and now > start + 200_000_000 and (self.directory / 'traffic.started').exists():
                raise OSError('injected evidence write failure after traffic')
            if self.spec['scene'] != 'steady' and dispatched < 4:
                planned = start + int((dispatched + 1) * self.spec['window_s'] / 5 * 1e9)
                if now >= planned:
                    if self.spec['scene'] == 'reorder':
                        self.configure([1, 0] if dispatched % 2 == 0 else [0, 1])
                    event = dict(type='hup', planned_ns=planned, ns=time.monotonic_ns(), config_sha256=sha(self.directory / 'runtime.conf'))
                    self.product.send_signal(signal.SIGHUP)
                    self.events.append(event)
                    dispatched += 1
            for event in self.events:
                if event['type'] == 'hup' and 'confirmed_ns' not in event:
                    status = 'applied' if self.spec['scene'] == 'reorder' else 'unchanged'
                    confirmation = next((row for row in self.events if row['type'] == 'output' and row['ns'] >= event['ns'] and row['line'].startswith('XDP_RELOAD ') and f'status={status}' in row['line']), None)
                    if confirmation:
                        event.update(confirmed_ns=confirmation['ns'], status=status, **active(self.product.pid, self.identity['libbpf']))
            if now > end + 2_000_000_000:
                raise RuntimeError('measurement drain deadline')
            time.sleep(.002)
        self.stop_sampling.set()
        self.sampler.join(1)
        self.check()
        (self.directory / 'client.stop').touch()
        return preflight, json.loads((self.directory / 'traffic.json').read_text())

    def close(self):
        self.stop_sampling.set()
        if hasattr(self, 'sampler'):
            self.sampler.join(1)
        # Product detaches before the namespaces disappear; then echo counters flush.
        if self.product and self.product.poll() is None:
            self.product.terminate()
            try:
                self.product.wait(timeout=1.5)
            except S.TimeoutExpired:
                self.product.kill()
                self.product.wait(timeout=1)
                self.errors.append('forced product kill')
        if self.product and self.spec['path'] not in ('direct', 'proxy'):
            attached = json.loads(self.ip(None, '-j', 'link', 'show', 'xb'))[0].get('xdp', {}).get('prog', {}).get('id', 0)
            if attached:
                self.errors.append('owned product still attached')
        ordered = [p for p in reversed(self.owned) if p is not self.product]
        for process in ordered:
            if process.poll() is None:
                process.terminate()
        deadline = time.monotonic() + 2
        for process in ordered:
            try:
                process.wait(timeout=max(.01, deadline - time.monotonic()))
            except S.TimeoutExpired:
                process.kill()
                process.wait(timeout=1)
                self.errors.append(f'forced kill {process.pid}')
        for thread in self.threads:
            thread.join(1)
        for handle in self.logs:
            handle.close()
        return dict(owned_reaped=all(p.poll() is not None for p in self.owned), remaining=[p.pid for p in self.owned if p.poll() is None],
                    children=[dict(pid=p.pid, returncode=p.returncode) for p in self.owned])
