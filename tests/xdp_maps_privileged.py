#!/usr/bin/env python3
"""Explicit V1.2/S1 kernel acceptance; only disposable network interfaces."""
import argparse
import ctypes as C
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import struct
import subprocess as S
import sys
import tempfile
import time


def command(*args, expected=0):
    result = S.run(args, capture_output=True, text=True, timeout=15)
    if result.returncode != expected:
        raise RuntimeError(f'{args}: rc={result.returncode}: {result.stdout} {result.stderr}')
    return result.stdout


class TestRun(C.Structure):
    _fields_ = [('sz', C.c_size_t), ('data_in', C.c_void_p),
                ('data_out', C.c_void_p), ('data_size_in', C.c_uint32),
                ('data_size_out', C.c_uint32), ('ctx_in', C.c_void_p),
                ('ctx_out', C.c_void_p), ('ctx_size_in', C.c_uint32),
                ('ctx_size_out', C.c_uint32), ('retval', C.c_uint32),
                ('repeat', C.c_int), ('duration', C.c_uint32),
                ('flags', C.c_uint32), ('cpu', C.c_uint32),
                ('batch_size', C.c_uint32)]


class Kernel:
    def __init__(self):
        self.lib = C.CDLL('libbpf.so.1', use_errno=True)
        declarations = {
            'bpf_map_get_fd_by_id': [C.c_uint32],
            'bpf_prog_get_fd_by_id': [C.c_uint32],
            'bpf_obj_get_info_by_fd': [C.c_int, C.c_void_p, C.POINTER(C.c_uint32)],
            'bpf_map_lookup_elem': [C.c_int, C.c_void_p, C.c_void_p],
            'bpf_map_update_elem': [C.c_int, C.c_void_p, C.c_void_p, C.c_uint64],
            'libbpf_num_possible_cpus': [],
            'bpf_prog_test_run_opts': [C.c_int, C.POINTER(TestRun)],
        }
        for name, parameters in declarations.items():
            method = getattr(self.lib, name)
            method.argtypes = parameters
            method.restype = C.c_int
        self.cpu_count = self.lib.libbpf_num_possible_cpus()
        assert self.cpu_count > 0

    def checked(self, name, *args):
        result = getattr(self.lib, name)(*args)
        if result < 0:
            raise OSError(C.get_errno(), name)
        return result

    def info(self, fd):
        buffer = C.create_string_buffer(256)
        size = C.c_uint32(len(buffer))
        self.checked('bpf_obj_get_info_by_fd', fd, buffer, C.byref(size))
        return buffer.raw

    def lookup(self, fd, key=0, size=8):
        index = C.c_uint32(key)
        value = C.create_string_buffer(size)
        self.checked('bpf_map_lookup_elem', fd, C.byref(index), value)
        return value.raw

    def test_run(self, fd, frame):
        source = C.create_string_buffer(frame)
        output = C.create_string_buffer(4096)
        options = TestRun(sz=C.sizeof(TestRun), data_in=C.addressof(source),
                          data_out=C.addressof(output), data_size_in=len(frame),
                          data_size_out=len(output), repeat=7)
        self.checked('bpf_prog_test_run_opts', fd, C.byref(options))
        assert options.retval == 2, options.retval
        assert output.raw[:options.data_size_out] == frame


class MapSnapshot:
    """Open only map IDs owned by the just-started product process."""
    def __init__(self, kernel, pid):
        self.kernel = kernel
        self.maps = {}
        try:
            identifiers = set()
            for path in Path(f'/proc/{pid}/fdinfo').iterdir():
                match = re.search(r'^map_id:\s+(\d+)$', path.read_text(), re.M)
                if match:
                    identifiers.add(int(match[1]))
            for ident in identifiers:
                fd = kernel.checked('bpf_map_get_fd_by_id', ident)
                try:
                    info = kernel.info(fd)
                    metadata = struct.unpack_from('=6I', info)
                    name = info[24:40].split(b'\0')[0].decode()
                    assert name not in self.maps
                    self.maps[name] = (fd, ident, metadata)
                except BaseException:
                    os.close(fd)
                    raise
            assert set(self.maps) == {'l4lb_cfg_v1', 'l4lb_be_v1', 'l4lb_stats_v1'}, self.maps
            for name, map_type, capacity, flags in [
                ('l4lb_cfg_v1', 2, 1, 128), ('l4lb_be_v1', 2, 64, 128),
                ('l4lb_stats_v1', 6, 1, 0),
            ]:
                _, ident, metadata = self.maps[name]
                assert metadata == (map_type, ident, 4, 8, capacity, flags), metadata
        except BaseException:
            self.close()
            raise

    def close(self):
        for fd, _, _ in self.maps.values():
            os.close(fd)
        self.maps.clear()

    def verify_configuration(self, backends):
        cfg = self.maps['l4lb_cfg_v1'][0]
        table = self.maps['l4lb_be_v1'][0]
        assert self.kernel.lookup(cfg) == struct.pack('=II', 1, len(backends))
        for index in range(64):
            expected = bytes(8)
            if index < len(backends):
                address, port = backends[index].split(':')
                expected = socket.inet_aton(address) + struct.pack('!H', int(port)) + bytes(2)
            assert self.kernel.lookup(table, index) == expected, index
        for fd in [cfg, table]:
            key, value = C.c_uint32(0), C.create_string_buffer(bytes(8))
            result = self.kernel.lib.bpf_map_update_elem(fd, C.byref(key), value, 0)
            assert result < 0 and C.get_errno() in [errno.EPERM, errno.EACCES]

    def count(self):
        value = self.kernel.lookup(self.maps['l4lb_stats_v1'][0],
                                   size=8 * self.kernel.cpu_count)
        return sum(struct.unpack(f'={self.kernel.cpu_count}Q', value)) % (1 << 64)


class Runner:
    def __init__(self, args, directory):
        self.args = args
        self.directory = Path(directory)
        self.processes = []
        self.logs = {}
        self.kernel = Kernel()
        command('ip', 'link', 'add', 'xa', 'type', 'veth', 'peer', 'name', 'xb')
        for device in ['lo', 'xa', 'xb']:
            command('ip', 'link', 'set', device, 'up')
        command('ip', 'addr', 'add', '198.18.0.2/24', 'dev', 'xb')
        for device in ['all', 'default', 'xb']:
            Path(f'/proc/sys/net/ipv4/conf/{device}/rp_filter').write_text('0')
        links = json.loads(command('ip', '-json', 'link', 'show'))
        macs = {link['ifname']: bytes.fromhex(link['address'].replace(':', '')) for link in links}
        self.payload = b'L4LB-V1.2-S1-PASS'
        udp = struct.pack('!HHHH', 39000, 39001, 8 + len(self.payload), 0) + self.payload
        header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0,
                             socket.inet_aton('198.18.0.1'), socket.inet_aton('198.18.0.2'))
        total = sum(struct.unpack('!10H', header))
        while total >> 16:
            total = (total & 65535) + (total >> 16)
        header = header[:10] + struct.pack('!H', (~total) & 65535) + header[12:]
        self.frame = macs['xb'] + macs['xa'] + b'\x08\x00' + header + udp

    def invocation(self, mode, backends=(), device='xb'):
        arguments = [str(self.args.loader), 'attach', '--dev', device, '--object',
                     str(self.args.object), '--maps', '--mode', mode]
        for backend in backends:
            arguments += ['--backend', backend]
        return arguments

    def start(self, mode, backends=(), device='xb', fault=None, closed_stdin=False,
              ready_failure=False, pipe_output=False):
        invocation = self.invocation(mode, backends, device)
        environment = os.environ.copy()
        if fault:
            environment.update(LD_PRELOAD=str(self.args.fault_library), L4LB_TEST_MAP_FAULT=fault)
        if closed_stdin:
            invocation = [sys.executable, '-c',
                          'import os,sys;os.close(0);os.execv(sys.argv[1],sys.argv[1:])', *invocation]
        log = self.directory / f'process-{len(self.processes)}.log'
        with log.open('w') as errors:
            if ready_failure:
                with open('/dev/full', 'w') as output:
                    process = S.Popen(invocation, stdout=output, stderr=errors, env=environment)
            else:
                process = S.Popen(invocation, stdout=S.PIPE if pipe_output else errors,
                                  stderr=errors, env=environment)
        self.processes.append(process)
        self.logs[process.pid] = log
        if ready_failure or (fault and fault != 'stats-read'):
            assert process.wait(timeout=8) == 1, log.read_text()
            assert not re.search(r'^READY ', log.read_text(), re.M)
            assert self.attached_id() == 0
            return process, 0
        deadline = time.monotonic() + 8
        with selectors.DefaultSelector() as selector:
            if pipe_output:
                selector.register(process.stdout, selectors.EVENT_READ)
            while time.monotonic() < deadline:
                if pipe_output:
                    text = process.stdout.readline().decode() if selector.select(0.05) else ''
                else:
                    text = log.read_text()
                match = re.search(r'READY .*prog_id=(\d+).*schema=1.*backend_count=(\d+)', text)
                if match:
                    assert int(match[2]) == len(backends)
                    return process, int(match[1])
                if process.poll() is not None:
                    raise RuntimeError(f'failed before READY: {log.read_text()}')
                time.sleep(0.02)
        raise RuntimeError(f'READY timeout: {log.read_text()}')

    def attached_id(self):
        link = json.loads(command('ip', '-json', '-details', 'link', 'show', 'dev', 'xb'))[0]
        return link.get('xdp', {}).get('prog', {}).get('id', 0)

    def traffic(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind(('198.18.0.2', 39001))
            receiver.settimeout(2)
            with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sender:
                sender.bind(('xa', 0))
                sender.send(self.frame)
            assert receiver.recv(4096) == self.payload

    def stop(self, process, sig=signal.SIGTERM, expected=0):
        process.send_signal(sig)
        assert process.wait(timeout=8) == expected, self.logs[process.pid].read_text()
        if expected == 0:
            text = self.logs[process.pid].read_text()
            match = re.search(r'XDP_STATS schema=1 pass_packets=(\d+)', text)
            assert match, text
            return int(match[1])

    def detach(self, mode, ident, expected=0):
        command(str(self.args.loader), 'detach', '--dev', 'xb', '--mode', mode,
                '--prog-id', str(ident), expected=expected)

    def released(self, ids, program_id):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            alive = []
            for name, ident in [('bpf_map_get_fd_by_id', item) for item in ids] + [
                ('bpf_prog_get_fd_by_id', program_id)
            ]:
                fd = getattr(self.kernel.lib, name)(ident)
                if fd >= 0:
                    os.close(fd)
                    alive.append(ident)
                else:
                    assert C.get_errno() == errno.ENOENT
            if not alive:
                return
            time.sleep(0.03)
        raise AssertionError(f'owned BPF resources still present: {alive}')

    def mode(self, mode):
        checks = []
        for count in [0, 1, 64]:
            backends = [f'10.0.0.{index + 1}:{8000 + index}' for index in range(count)]
            process, ident = self.start(mode, backends)
            snapshot = MapSnapshot(self.kernel, process.pid)
            ids = [entry[1] for entry in snapshot.maps.values()]
            try:
                snapshot.verify_configuration(backends)
                before = snapshot.count()
                self.traffic()
                assert snapshot.count() >= before + 1
                total = self.stop(process)
                assert total == snapshot.count()
            finally:
                snapshot.close()
            self.released(ids, ident)
            assert self.attached_id() == 0
            checks.append(f'backends_{count}_readback_freeze_count_cleanup')
        process, ident = self.start(mode)
        snapshot = MapSnapshot(self.kernel, process.pid)
        program_fd = self.kernel.checked('bpf_prog_get_fd_by_id', ident)
        try:
            self.detach(mode, ident)
            time.sleep(0.05)
            before = snapshot.count()
            self.kernel.test_run(program_fd, self.frame)
            assert snapshot.count() == before + 7
            assert self.stop(process) == snapshot.count()
        finally:
            os.close(program_fd)
            snapshot.close()
        checks.append('detached_test_run_exact_count_and_frame_unchanged')

        for fault in ['backend-write', 'config-write', 'config-read-error', 'config-mismatch',
                      'backend-mismatch', 'backend-freeze', 'config-freeze']:
            self.start(mode, ['10.0.0.1:8000', '10.0.0.2:8001'], fault=fault)
            self.traffic()
            checks.append(f'{fault}_no_attach')
        process, ident = self.start(mode, fault='stats-read')
        self.stop(process, expected=1)
        assert self.attached_id() == 0
        checks.append('stats_read_failure_still_detaches')

        process, ident = self.start(mode)
        command(*self.invocation(mode), expected=1)
        self.detach(mode, 4294967295, expected=1)
        assert self.attached_id() == ident
        self.stop(process, signal.SIGINT)
        assert self.attached_id() == 0
        checks += ['duplicate_and_wrong_id_preserved', 'sigint_detach']

        process, ident = self.start(mode, closed_stdin=True)
        command(sys.executable, '-c',
                'import os,sys;os.close(0);os.execv(sys.argv[1],sys.argv[1:])',
                str(self.args.loader), 'detach', '--dev', 'xb', '--mode', mode,
                '--prog-id', str(ident))
        self.detach(mode, ident)
        replacement, replacement_id = self.start(mode)
        self.stop(process, expected=1)
        assert self.attached_id() == replacement_id
        self.traffic()
        self.stop(replacement)
        process, ident = self.start(mode, closed_stdin=True)
        self.stop(process)
        assert self.attached_id() == 0
        checks.append('closed_stdin_explicit_idempotent_detach_replacement_protection')

        self.start(mode, ready_failure=True)
        assert self.attached_id() == 0
        checks.append('ready_output_failure_cleanup')
        process, ident = self.start(mode, pipe_output=True)
        process.stdout.close()
        self.stop(process, expected=1)
        assert self.attached_id() == 0
        checks.append('stop_output_failure_cleanup')

        process, ident = self.start(mode)
        snapshot = MapSnapshot(self.kernel, process.pid)
        ids = [entry[1] for entry in snapshot.maps.values()]
        snapshot.close()
        process.kill()
        assert process.wait(timeout=8) == -signal.SIGKILL
        assert self.attached_id() == ident
        self.detach(mode, ident)
        self.released(ids, ident)
        checks.append('sigkill_explicit_recovery_and_map_cleanup')

        command('ip', 'link', 'add', 'xc', 'type', 'veth', 'peer', 'name', 'xd')
        for device in ['xc', 'xd']:
            command('ip', 'link', 'set', device, 'up')
        process, ident = self.start(mode, device='xc')
        command('ip', 'link', 'delete', 'xc')
        self.stop(process)
        self.traffic()
        checks.append('device_removed_cleanup')
        return checks

    def close(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except S.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            if process.stdout:
                process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', required=True, type=Path)
    parser.add_argument('--object', required=True, type=Path)
    parser.add_argument('--fault-library', required=True, type=Path,
                        help='test-only shared library compiled from tests/XdpMapFaults.c')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    for name in ['loader', 'object', 'fault_library']:
        setattr(args, name, getattr(args, name).resolve(strict=True))
    if os.geteuid() != 0:
        parser.error('requires explicit root; the runner creates an isolated net namespace')
    if not args.inside:
        return S.call(['unshare', '--net', sys.executable, str(Path(__file__).resolve()),
                       '--inside', '--loader', str(args.loader), '--object', str(args.object),
                       '--fault-library', str(args.fault_library)])
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
    paths = [args.loader, args.object, args.fault_library, Path(__file__),
             Path(__file__).with_name('XdpMapFaults.c')]
    result = {'scope': 'V1.2/S1 maps; no forwarding or performance claim',
              'kernel': command('uname', '-srmo').strip(),
              'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
              'modes': {}}
    with tempfile.TemporaryDirectory(prefix='l4lb-maps-') as directory:
        runner = Runner(args, directory)
        try:
            for mode in ['generic', 'native']:
                checks = runner.mode(mode)
                result['modes'][mode] = {'status': 'PASS', 'checks': checks}
                print(f'{mode}: PASS ({len(checks)} checks)', flush=True)
        finally:
            runner.close()
        assert all(process.poll() is not None for process in runner.processes)
        assert runner.attached_id() == 0
    print(json.dumps(result, indent=2), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
