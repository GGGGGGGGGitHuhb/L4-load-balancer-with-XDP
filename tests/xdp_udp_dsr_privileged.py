#!/usr/bin/env python3
"""S2 real BPF and isolated two-backend DSR acceptance; never changes host networking."""
import argparse
import ctypes as C
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import select
import socket
import struct
import subprocess as S
import sys
import time
from xdp_maps_privileged import Kernel, TestRun, command

VIP = '198.19.0.100'
PORT = 39001


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def frame(port=40000, destination=VIP, udp_checksum=False, **changes):
    payload = changes.get('payload', b'actual-kernel-dsr')
    src, dst = socket.inet_aton('198.18.0.1'), socket.inet_aton(destination)
    udp = struct.pack('!HHHH', port, changes.get('dport', PORT), 8 + len(payload), 0) + payload
    if udp_checksum:
        value = checksum(src + dst + struct.pack('!BBH', 0, 17, len(udp)) + udp) or 65535
        udp = udp[:6] + struct.pack('!H', value) + udp[8:]
    ip = struct.pack('!BBHHHBBH4s4s', changes.get('version', 0x45), 0,
                     changes.get('length', 20 + len(udp)), 9, changes.get('fragment', 0),
                     64, changes.get('protocol', 17), 0, src, dst)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    return bytes.fromhex('0200000000020200000000010800') + ip + udp


def reference_hash(packet):
    value = 2166136261
    for byte in packet[26:34] + packet[34:38] + packet[23:24]:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


class Object:
    def __init__(self, kernel, path):
        self.kernel = kernel
        lib = kernel.lib
        for name, args, result in [
            ('bpf_object__open_file', [C.c_char_p, C.c_void_p], C.c_void_p),
            ('libbpf_get_error', [C.c_void_p], C.c_long),
            ('bpf_object__load', [C.c_void_p], C.c_int),
            ('bpf_object__close', [C.c_void_p], None),
            ('bpf_object__find_map_fd_by_name', [C.c_void_p, C.c_char_p], C.c_int),
            ('bpf_object__find_program_by_name', [C.c_void_p, C.c_char_p], C.c_void_p),
            ('bpf_program__fd', [C.c_void_p], C.c_int),
        ]:
            function = getattr(lib, name)
            function.argtypes, function.restype = args, result
        self.obj = lib.bpf_object__open_file(os.fsencode(path), None)
        assert self.obj and not lib.libbpf_get_error(self.obj)
        kernel.checked('bpf_object__load', self.obj)
        self.maps = {name: lib.bpf_object__find_map_fd_by_name(self.obj, name.encode())
                     for name in ['l4lb_cfg_v2', 'l4lb_be_v2', 'l4lb_stats_v2']}
        self.fd = lib.bpf_program__fd(lib.bpf_object__find_program_by_name(self.obj, b'xdp_udp_dsr'))
        assert min(self.maps.values()) >= 0 and self.fd >= 0

    def write(self, name, value, index=0):
        key, data = C.c_uint32(index), C.create_string_buffer(value)
        self.kernel.checked('bpf_map_update_elem', self.maps[name], C.byref(key), data, 0)

    def configure(self, count=2, version=2, reserved=0):
        self.write('l4lb_cfg_v2', struct.pack('=II', version, count) +
                   socket.inet_aton(VIP) + struct.pack('!H', PORT) + struct.pack('=H', reserved))
        for index in range(64):
            self.write('l4lb_be_v2', backend(index), index)

    def stats(self):
        data = self.kernel.lookup(self.maps['l4lb_stats_v2'], size=64 * self.kernel.cpu_count)
        return [sum(struct.unpack_from('=8Q', data, cpu * 64)[i]
                    for cpu in range(self.kernel.cpu_count)) for i in range(8)]

    def run(self, packet, action=2, reason=4, expected=None):
        before = self.stats()
        source, output = C.create_string_buffer(packet), C.create_string_buffer(4096)
        options = TestRun(sz=C.sizeof(TestRun), data_in=C.addressof(source), data_out=C.addressof(output),
                          data_size_in=len(packet), data_size_out=len(output), repeat=1)
        self.kernel.checked('bpf_prog_test_run_opts', self.fd, C.byref(options))
        assert options.retval == action, (options.retval, action, packet.hex())
        assert output.raw[:options.data_size_out] == (packet if expected is None else expected)
        delta = [a - b for a, b in zip(self.stats(), before)]
        wanted = [0] * 8
        wanted[0] = 1
        wanted[{2: 1, 4: 2, 1: 3}[action]] = 1
        if reason is not None:
            wanted[reason] = 1
        assert delta == wanted, (delta, wanted)

    def close(self):
        self.kernel.lib.bpf_object__close(self.obj)


def backend(index):
    return struct.pack('=I', 100 + index) + bytes([2, 0, 0, 1, 0, index]) + bytes([2, 0, 0, 2, 0, index])


def kernel_cases(args):
    obj = Object(Kernel(), args.object)
    count = 0
    try:
        obj.configure()
        packet = frame()
        assert reference_hash(packet) == 2362691501
        assert reference_hash(frame(40001)) == 1344874376
        # Linux test-run rejects frames shorter than Ethernet before calling BPF.
        for length in range(14, 42):
            obj.run(packet[:length]); count += 1
        unsupported = [frame(protocol=6), frame(version=0x65), frame(version=0x46),
                       frame(fragment=0x8000), frame(fragment=0x2000), frame(fragment=1),
                       frame(length=27), frame(length=1501), frame(length=80),
                       frame(destination='198.19.0.101'), frame(dport=PORT + 1)]
        unsupported += [packet[:12] + kind + packet[14:] for kind in [b'\x81\x00', b'\x86\xdd', b'\x88\xa8']]
        unsupported += [packet[:24] + b'\x00\x00' + packet[26:],
                        packet[:38] + struct.pack('!H', 7) + packet[40:],
                        packet[:38] + struct.pack('!H', 9) + packet[40:]]
        for item in unsupported:
            obj.run(item); count += 1
        for total in [0, 1, 2, 64]:
            obj.configure(total)
            seen = set()
            for port in range(40000, 40256):
                item = frame(port, udp_checksum=bool(port % 2))
                if port % 3 == 0:
                    item += b'padding'
                if total == 0:
                    obj.run(item, reason=5)
                else:
                    index = reference_hash(item) % total
                    seen.add(index)
                    mac = backend(index)[4:]
                    obj.run(item, action=4, reason=None, expected=mac + item[12:])
                count += 1
            assert total == 0 or len(seen) == total, (total, seen)
        for kw in [{'version': 1}, {'count': 65}, {'reserved': 1}]:
            obj.configure(**kw); obj.run(packet, reason=6); count += 1
        obj.configure(1)
        for value in [bytes(16), struct.pack('=I', 100) + bytes(12),
                      struct.pack('=I', 100) + bytes.fromhex('030000000001020000000002'),
                      struct.pack('=I', 100) + bytes.fromhex('020000000001ffffffffffff')]:
            obj.write('l4lb_be_v2', value)
            obj.run(packet, reason=6); count += 1
    finally:
        obj.close()
    if args.helper_error_object:
        obj = Object(Kernel(), args.helper_error_object)
        try:
            obj.configure(1)
            obj.run(frame(), action=1, reason=7)
            count += 1
        finally:
            obj.close()
    obj = Object(Kernel(), args.short_bound_object)
    try:
        obj.configure(2)
        for length in range(14):
            obj.run(bytes([length]) + frame()[1:])
    finally:
        obj.close()
    return {'test_run_cases': count, 'short_logical_bound_fixture_cases': 14, 'ethernet_shorter_than_14': 'kernel test-run API rejects before execution'}


SERVER = r'''
import json,socket,sys,threading
vip,port,identity,device,log=sys.argv[1:]
def capture():
 s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(0x800));s.bind((device,0))
 while True:
  p=s.recv(65535)
  if len(p)>=42 and p[23]==17:
   with open(log,'a') as f:f.write(json.dumps({'mac':p[:12].hex(),'ip':p[14:].hex()})+'\n')
threading.Thread(target=capture,daemon=True).start()
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind((vip,int(port)))
print('SERVER_READY',flush=True)
while True:
 data,peer=s.recvfrom(4096);print('RECEIVED',peer,flush=True);s.sendto(identity.encode()+b':'+data,peer)
'''
CLIENT = r'''
import json,socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(float(sys.argv[4]));s.bind(('198.18.0.1',int(sys.argv[1])))
answers=[]
for n in range(2):
 s.sendto(b'dsr-roundtrip',(sys.argv[2],int(sys.argv[3])))
 try:
  data,peer=s.recvfrom(4096);answers.append([data.decode(),list(peer)])
 except TimeoutError:answers.append(None)
print(json.dumps(answers))
'''


class Topology:
    def __init__(self, args):
        self.args, self.processes, self.namespaces = args, [], {}
        self.logs = {}
        self.directory = args.log_dir
        for name in ['client', 'be0', 'be1']:
            process = S.Popen(['unshare', '--net', 'sleep', '3600'])
            self.processes.append(process)
            for _ in range(100):
                if os.readlink(f'/proc/{process.pid}/ns/net') != os.readlink('/proc/self/ns/net'):
                    break
                time.sleep(.01)
            else:
                raise RuntimeError('namespace startup timeout')
            self.namespaces[name] = process.pid
            self.ip(name, 'link', 'set', 'lo', 'up')
        self.pair('xb', 'xa', 'client')
        self.ip(None, 'addr', 'add', '198.18.0.2/24', 'dev', 'xb')
        self.ip('client', 'addr', 'add', '198.18.0.1/24', 'dev', 'xa')
        # veth partial TX checksums are not valid wire frames after XDP redirect.
        self.call('client', 'ethtool', '-K', 'xa', 'tx', 'off')
        self.ip('client', 'route', 'add', VIP + '/32', 'via', '198.18.0.2')
        self.ip('client', 'neigh', 'replace', '198.18.0.2', 'lladdr', self.mac(None, 'xb'), 'nud', 'permanent', 'dev', 'xa')
        for i in range(2):
            name = f'be{i}'
            self.pair(f'e{i}', f'b{i}', name)
            self.ip(name, 'addr', 'add', f'198.20.{i}.2/24', 'dev', f'b{i}')
            self.pair(f'q{i}', f'r{i}', name, 'client')
            self.ip('client', 'addr', 'add', f'198.18.{i+1}.1/24', 'dev', f'q{i}')
            self.ip(name, 'addr', 'add', f'198.18.{i+1}.2/24', 'dev', f'r{i}')
            self.ip(name, 'addr', 'add', VIP + '/32', 'dev', 'lo')
            self.ip(name, 'route', 'add', '198.18.0.1/32', 'via', f'198.18.{i+1}.1')
            self.ip(name, 'neigh', 'replace', f'198.18.{i+1}.1', 'lladdr', self.mac('client', f'q{i}'), 'nud', 'permanent', 'dev', f'r{i}')
        for name in [None, 'client', 'be0', 'be1']:
            self.call(name, sys.executable, '-c',
                      "from pathlib import Path;[p.write_text('0') for p in Path('/proc/sys/net/ipv4/conf').glob('*/rp_filter')]")
        for i in range(2):
            log = self.directory / f'capture-{i}.jsonl'
            self.server(f'be{i}', str(i), f'b{i}', VIP, PORT, log)
        self.server(None, 'host', 'xb', '198.18.0.2', PORT+1, self.directory/'host-capture.jsonl')

    def prefix(self, name):
        return [] if name is None else ['nsenter', '-t', str(self.namespaces[name]), '-n']

    def call(self, name, *args, expected=0):
        return command(*self.prefix(name), *map(str, args), expected=expected)

    def ip(self, name, *args):
        return self.call(name, 'ip', *args)

    def pair(self, local, remote, namespace, local_namespace=None):
        self.ip(None, 'link', 'add', local, 'type', 'veth', 'peer', 'name', remote)
        self.ip(None, 'link', 'set', remote, 'netns', str(self.namespaces[namespace]))
        if local_namespace:
            self.ip(None, 'link', 'set', local, 'netns', str(self.namespaces[local_namespace]))
        self.ip(local_namespace, 'link', 'set', local, 'up')
        self.ip(namespace, 'link', 'set', remote, 'up')

    def mac(self, name, device):
        return json.loads(self.ip(name, '-j', 'link', 'show', device))[0]['address']

    def server(self, name, identity, device, vip, port, log):
        handle = (self.directory / f'server-{identity}.log').open('w')
        process = S.Popen([*self.prefix(name), sys.executable, '-u', '-c', SERVER,
                          vip, str(port), identity, device, str(log)], stdout=handle, stderr=handle)
        handle.close()
        self.processes.append(process)
        for _ in range(100):
            if 'SERVER_READY' in (self.directory / f'server-{identity}.log').read_text():
                return
            time.sleep(.02)
        raise RuntimeError('UDP server did not become ready')

    def invocation(self, mode, device='xb'):
        args = [str(self.args.loader), 'attach', '--dev', device, '--object', str(self.args.object),
                '--mode', mode, '--udp-dsr', '--vip', f'{VIP}:{PORT}']
        for i in range(2):
            args += ['--target', f'e{i}@{self.mac(f"be{i}", f"b{i}")}']
        return args

    def attached(self):
        return json.loads(self.ip(None, '-j', '-d', 'link', 'show', 'xb'))[0].get('xdp', {}).get('prog', {}).get('id', 0)

    def start(self, mode, closed=False, output_failure=False, fault=None, pipe=False, device='xb'):
        log = self.directory / f'loader-{len(self.processes)}.log'
        invocation = self.invocation(mode, device)
        if closed:
            invocation = [sys.executable, '-c', 'import os,sys;os.close(0);os.execv(sys.argv[1],sys.argv[1:])', *invocation]
        environment = os.environ.copy()
        if fault:
            environment.update(LD_PRELOAD=str(self.args.fault_library), L4LB_TEST_MAP_FAULT=fault)
        with log.open('w') as handle, open('/dev/full', 'w') as full:
            process = S.Popen(invocation, stdout=full if output_failure else (S.PIPE if pipe else handle), stderr=handle, env=environment)
        self.processes.append(process); self.logs[process.pid] = log
        if output_failure or (fault and fault != 'stats-read'):
            assert process.wait(timeout=10) == 1, log.read_text()
            assert not re.search(r'^READY ', log.read_text(), re.M) and self.attached() == 0
            return process, 0
        for _ in range(400):
            text = (process.stdout.readline().decode() if select.select([process.stdout], [], [], .02)[0] else '') if pipe else log.read_text()
            match = re.search(r'READY .*prog_id=(\d+).*schema=2', text)
            if match:
                return process, int(match[1])
            if process.poll() is not None:
                raise RuntimeError(log.read_text())
            time.sleep(.02)
        raise RuntimeError('loader READY timeout: ' + log.read_text())

    def stop(self, process, sig=signal.SIGTERM, expected=0):
        process.send_signal(sig)
        assert process.wait(timeout=10) == expected, self.logs[process.pid].read_text()
        if process.stdout:
            process.stdout.close()

    def detach(self, mode, ident, expected=0):
        self.call(None, self.args.loader, 'detach', '--dev', 'xb', '--mode', mode, '--prog-id', ident, expected=expected)

    def client(self, port, vip=VIP, dport=PORT, timeout=2):
        return json.loads(self.call('client', sys.executable, '-c', CLIENT, port, vip, dport, timeout))

    def snapshot(self, process):
        kernel = Kernel()
        found = {}
        try:
            for path in Path(f'/proc/{process.pid}/fdinfo').iterdir():
                match = re.search(r'^map_id:\s+(\d+)$', path.read_text(), re.M)
                if not match:
                    continue
                ident = int(match[1])
                fd = kernel.checked('bpf_map_get_fd_by_id', ident)
                info = kernel.info(fd)
                name = info[24:40].split(b'\0')[0].decode()
                if name in found:
                    os.close(fd)
                    continue
                found[name] = fd
                size = 64 if name == 'l4lb_stats_v2' else 16
                assert struct.unpack_from('=6I', info) == (
                    6 if size == 64 else 2, ident, 4, size,
                    64 if name == 'l4lb_be_v2' else 1, 0 if size == 64 else 128)
            assert set(found) == {'l4lb_cfg_v2', 'l4lb_be_v2', 'l4lb_stats_v2'}
            expected = struct.pack('=II', 2, 2) + socket.inet_aton(VIP) + struct.pack('!H', PORT) + bytes(2)
            assert kernel.lookup(found['l4lb_cfg_v2'], size=16) == expected
            for i in range(64):
                expected = bytes(16)
                if i < 2:
                    index = json.loads(self.ip(None, '-j', 'link', 'show', f'e{i}'))[0]['ifindex']
                    expected = struct.pack('=I', index) + bytes.fromhex((self.mac(f'be{i}', f'b{i}') + self.mac(None, f'e{i}')).replace(':', ''))
                assert kernel.lookup(found['l4lb_be_v2'], i, 16) == expected
            for name in ['l4lb_cfg_v2', 'l4lb_be_v2']:
                key, value = C.c_uint32(0), C.create_string_buffer(bytes(16))
                assert kernel.lib.bpf_map_update_elem(found[name], C.byref(key), value, 0) < 0
                assert C.get_errno() in [errno.EACCES, errno.EPERM]
            data = kernel.lookup(found['l4lb_stats_v2'], size=64 * kernel.cpu_count)
            values = [sum(struct.unpack_from('=8Q', data, cpu*64)[i] for cpu in range(kernel.cpu_count)) for i in range(8)]
            assert values[0] == values[1] + values[2] + values[3]
            return values
        finally:
            for fd in found.values():
                os.close(fd)

    def interface_rejections(self, mode):
        base = self.invocation(mode)
        def reject(args):
            result = S.run(args, capture_output=True, text=True, timeout=8)
            assert result.returncode in [1, 2] and not re.search(r'^READY ', result.stdout, re.M)
            assert self.attached() == 0
        reject(base + ['--target', base[-1]])
        for target in ['lo@02:00:00:00:00:01', 'missing@02:00:00:00:00:01', 'xb@'+self.mac(None, 'xb')]:
            reject(base[:-4] + ['--target', target])
        self.ip(None, 'link', 'set', 'e0', 'mtu', '1400')
        reject(base)
        self.ip(None, 'link', 'set', 'e0', 'mtu', '1500')
        self.ip(None, 'link', 'set', 'e0', 'down')
        reject(base)
        self.ip(None, 'link', 'set', 'e0', 'up')

    def run(self, mode):
        checks = []
        if mode == 'native':
            # veth ndo_xdp_xmit requires its peer to have native XDP/NAPI enabled.
            for index in range(2):
                self.ip(f'be{index}', 'link', 'set', f'b{index}', 'xdpdrv', 'obj', str(self.args.pass_object), 'sec', 'xdp')
        self.interface_rejections(mode)
        for index in range(2):
            (self.directory / f'capture-{index}.jsonl').write_text('')
        process, ident = self.start(mode)
        self.snapshot(process)
        selected = {}
        for port in range(40000, 40016):
            index = reference_hash(frame(port)) % 2
            answers = self.client(port)
            assert answers == [[f'{index}:dsr-roundtrip', [VIP, PORT]]] * 2, (answers, self.call(f'be{index}', 'cat', '/proc/net/snmp'), self.call('client', 'cat', '/proc/net/snmp'))
            selected[index] = port
        assert len(selected) == 2
        for i in range(2):
            rows = [json.loads(line) for line in (self.directory/f'capture-{i}.jsonl').read_text().splitlines()]
            assert rows
            expected_mac = (self.mac(f'be{i}', f'b{i}') + self.mac(None, f'e{i}')).replace(':', '')
            assert all(row['mac'] == expected_mac for row in rows), rows
            assert all(bytes.fromhex(row['ip'])[12:20] == socket.inet_aton('198.18.0.1') + socket.inet_aton(VIP) for row in rows)
        assert self.client(41000, '198.18.0.2', PORT+1) == [['host:dsr-roundtrip', ['198.18.0.2', PORT+1]]] * 2
        stats = self.snapshot(process)
        assert stats[2] >= 32 and stats[1] >= 2, stats
        checks += ['real_full_map_readback_freeze_metadata', 'interface_validation_negatives', 'two_backend_request_response_and_mac_capture', 'stable_hash_and_reply_source', 'host_pass_delivery']
        command(*self.invocation(mode), expected=1)
        self.detach(mode, 4294967295, expected=1)
        assert self.attached() == ident
        self.ip(None, 'link', 'set', 'e0', 'down')
        assert self.client(selected[0], timeout=.2) == [None, None]
        self.ip(None, 'link', 'set', 'e0', 'up')
        assert self.client(selected[0])[0][0].startswith('0:')
        self.detach(mode, ident)
        stopped_stats = self.snapshot(process)
        self.stop(process, signal.SIGINT); assert self.attached() == 0
        text = self.logs[process.pid].read_text()
        fields = ['total_packets', 'pass_packets', 'redirect_requests', 'drop_packets', 'unsupported_packets', 'no_backend_packets', 'invalid_config_packets', 'helper_error_packets']
        printed = [int(re.search(r'\b' + field + r'=(\d+)', text)[1]) for field in fields]
        assert printed == stopped_stats, (printed, stopped_stats)
        checks.append('stop_stats_equal_independent_kernel_sum')
        checks += ['duplicate_wrong_id_preserved', 'egress_down_no_delivery_recovery', 'sigint_cleanup']
        process, ident = self.start(mode, closed=True)
        self.detach(mode, ident)
        replacement, new_id = self.start(mode)
        self.stop(process, expected=1)
        assert self.attached() == new_id
        self.stop(replacement)
        self.start(mode, output_failure=True)
        process, ident = self.start(mode, pipe=True)
        process.stdout.close(); self.stop(process, expected=1)
        process, ident = self.start(mode)
        process.kill(); assert process.wait(timeout=5) == -signal.SIGKILL
        assert self.attached() == ident
        self.detach(mode, ident); assert self.attached() == 0
        checks += ['closed_stdin_replacement_protection', 'output_failure_cleanup', 'sigkill_recovery']
        if self.args.fault_library:
            for fault in ['backend-write', 'config-write', 'config-read-error', 'config-mismatch', 'backend-mismatch', 'backend-freeze', 'config-freeze']:
                self.start(mode, fault=fault)
            process, _ = self.start(mode, fault='stats-read')
            self.stop(process, expected=1); assert self.attached() == 0
            checks += ['sync_readback_freeze_failures_no_attach', 'stats_failure_detaches']
        process, ident = self.start(mode)
        self.ip(None, 'link', 'delete', 'e0')
        assert self.client(selected[0], timeout=.2) == [None, None]
        self.stop(process); assert self.attached() == 0
        self.pair('e0', 'b0', 'be0')
        self.ip('be0', 'addr', 'add', '198.20.0.2/24', 'dev', 'b0')
        self.call('be0', sys.executable, '-c', "from pathlib import Path;Path('/proc/sys/net/ipv4/conf/b0/rp_filter').write_text('0')")
        if mode == 'generic':
            self.server('be0', '0-capture', 'b0', '198.20.0.2', PORT+2, self.directory/'capture-0.jsonl')
        checks += ['egress_deleted_no_delivery_cleanup']
        self.pair('gone', 'gonepeer', 'client')
        process, _ = self.start(mode, device='gone')
        self.ip(None, 'link', 'delete', 'gone')
        self.stop(process)
        checks += ['ingress_deleted_cleanup']
        return checks

    def close(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except S.TimeoutExpired:
                    process.kill(); process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--helper-error-object', type=Path, required=True)
    parser.add_argument('--short-bound-object', type=Path, required=True)
    parser.add_argument('--pass-object', type=Path, required=True)
    parser.add_argument('--fault-library', type=Path, required=True)
    parser.add_argument('--log-dir', type=Path, required=True, help='Persistent failure logs; use a fresh role directory')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    for key in ['loader', 'object', 'helper_error_object', 'short_bound_object', 'pass_object', 'fault_library']:
        if getattr(args, key):
            setattr(args, key, getattr(args, key).resolve(strict=True))
    args.log_dir = args.log_dir.resolve()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    if os.geteuid() != 0:
        parser.error('requires root; creates isolated namespaces and never modifies host interfaces')
    if not args.inside:
        before = command('ip', '-j', 'link'), command('ip', '-j', 'route')
        result = S.call(['unshare', '--net', sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--inside'])
        assert before == (command('ip', '-j', 'link'), command('ip', '-j', 'route')), 'host network changed'
        return result
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
    result = {'kernel': command('uname', '-srmo').strip(), 'scope': 'functional veth only; no performance or physical NIC claim',
              'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in [args.loader, args.object, args.pass_object, args.helper_error_object, args.short_bound_object, args.fault_library, Path(__file__), Path(__file__).with_name('build_xdp_dsr_fixtures.py'), Path(__file__).with_name('XdpDsrFaults.c')]}}
    result['kernel_cases'] = kernel_cases(args)
    topology = None
    try:
        topology = Topology.__new__(Topology)
        topology.__init__(args)
        result['modes'] = {mode: topology.run(mode) for mode in ['generic', 'native']}
    finally:
        if topology:
            topology.close()
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
