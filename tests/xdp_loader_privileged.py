#!/usr/bin/env python3
"""Explicit root-only product validation in a disposable network namespace."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess as S
import sys
import tempfile
import time


def command(*args, expected=0):
    result = S.run(args, capture_output=True, text=True, timeout=10)
    if result.returncode != expected:
        raise RuntimeError(f'{args}: rc={result.returncode}, {result.stdout} {result.stderr}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.loader = args.loader.resolve(strict=True)
    args.object = args.object.resolve(strict=True)
    if os.geteuid() != 0:
        parser.error('requires sudo; the runner creates its own disposable net namespace')
    if not args.inside:
        return S.call(['unshare', '--net', sys.executable, str(Path(__file__).resolve()),
                       '--inside', '--loader', str(args.loader), '--object', str(args.object)])
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net'), 'isolated netns required'
    command('ip', 'link', 'add', 'xa', 'type', 'veth', 'peer', 'name', 'xb')
    for device in ['lo', 'xa', 'xb']:
        command('ip', 'link', 'set', device, 'up')
    command('ip', 'addr', 'add', '198.18.0.2/24', 'dev', 'xb')
    for device in ['all', 'default', 'xb']:
        Path(f'/proc/sys/net/ipv4/conf/{device}/rp_filter').write_text('0')
    links = json.loads(command('ip', '-json', 'link', 'show'))
    macs = {i['ifname']: bytes.fromhex(i['address'].replace(':', '')) for i in links}
    payload = b'L4LB-S2-product-XDP-PASS'
    udp = struct.pack('!HHHH', 39000, 39001, 8 + len(payload), 0) + payload
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0,
                         socket.inet_aton('198.18.0.1'), socket.inet_aton('198.18.0.2'))
    total = sum(struct.unpack('!10H', header))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    header = header[:10] + struct.pack('!H', (~total) & 65535) + header[12:]
    frame = macs['xb'] + macs['xa'] + b'\x08\x00' + header + udp
    results = {'kernel': command('uname', '-srmo').strip(),
               'scope': 'V1.1/S2 product loader; no physical NIC or performance claim',
               'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [args.loader, args.object, Path(__file__)]}, 'modes': {}}

    with tempfile.TemporaryDirectory(prefix='l4lb-xdp-root-') as directory:
        processes = []
        serial = 0

        def start(mode, output_failure=False, closed_stdin=False, device="xb"):
            nonlocal serial
            serial += 1
            log = Path(directory) / f'process-{serial}.log'
            invocation = [str(args.loader), 'attach', '--dev', device, '--object',
                          str(args.object), '--mode', mode]
            if closed_stdin:
                invocation = [sys.executable, '-c',
                              'import os,sys; os.close(0); os.execv(sys.argv[1],sys.argv[1:])',
                              *invocation]
            with log.open('w') as errors:
                if output_failure:
                    with open('/dev/full', 'w') as output:
                        process = S.Popen(invocation, stdout=output, stderr=errors)
                else:
                    process = S.Popen(invocation, stdout=errors, stderr=errors)
            processes.append(process)
            if output_failure:
                assert process.wait(timeout=5) == 1, log.read_text()
                return process, 0
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                text = log.read_text()
                match = re.search(r'READY .*prog_id=(\d+)', text)
                if match:
                    return process, int(match[1])
                if process.poll() is not None:
                    raise RuntimeError(f'loader failed before READY: {text}')
                time.sleep(0.02)
            raise RuntimeError(f'loader READY timeout: {log.read_text()}')

        def attached_id():
            link = json.loads(command('ip', '-json', '-details', 'link', 'show', 'dev', 'xb'))[0]
            return link.get('xdp', {}).get('prog', {}).get('id', 0)

        def traffic():
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
                receiver.bind(('198.18.0.2', 39001))
                receiver.settimeout(2)
                with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as sender:
                    sender.bind(('xa', 0))
                    sender.send(frame)
                assert receiver.recv(4096) == payload

        def stop(process, sig=signal.SIGTERM, expected=0):
            process.send_signal(sig)
            assert process.wait(timeout=5) == expected

        def detach(mode, program_id, expected=0):
            return command(str(args.loader), 'detach', '--dev', 'xb', '--mode', mode,
                           '--prog-id', str(program_id), expected=expected)

        try:
            for mode in ['generic', 'native']:
                checks = []
                process, ident = start(mode)
                assert attached_id() == ident and ident > 0
                traffic()
                checks += ['attach_id', 'udp_pass']
                command(str(args.loader), 'attach', '--dev', 'xb', '--object', str(args.object),
                        '--mode', mode, expected=1)
                assert attached_id() == ident
                checks.append('duplicate_preserved')
                detach(mode, 4294967295, expected=1)
                assert attached_id() == ident
                checks.append('wrong_id_preserved')
                stop(process)
                assert attached_id() == 0
                traffic()
                checks += ['sigterm_detach', 'udp_after_detach']
                process, ident = start(mode)
                stop(process, signal.SIGINT)
                assert attached_id() == 0
                checks.append('sigint_detach')
                process, ident = start(mode)
                detach(mode, ident)
                detach(mode, ident)
                assert attached_id() == 0
                replacement, new_id = start(mode)
                assert new_id != ident
                stop(process, expected=1)
                assert attached_id() == new_id
                traffic()
                stop(replacement)
                checks += ['explicit_detach', 'idempotent_detach', 'replacement_preserved']
                start(mode, output_failure=True)
                assert attached_id() == 0
                checks.append('ready_output_failure_cleanup')
                process, ident = start(mode)
                process.kill()
                assert process.wait(timeout=5) == -signal.SIGKILL
                assert attached_id() == ident
                detach(mode, ident)
                assert attached_id() == 0
                checks.append('sigkill_explicit_recovery')
                process, ident = start(mode, closed_stdin=True)
                command(sys.executable, '-c',
                        'import os,sys; os.close(0); os.execv(sys.argv[1],sys.argv[1:])',
                        str(args.loader), 'detach', '--dev', 'xb', '--mode', mode,
                        '--prog-id', str(ident))
                assert attached_id() == 0
                replacement, new_id = start(mode)
                stop(process, expected=1)
                assert attached_id() == new_id
                stop(replacement)
                process, ident = start(mode, closed_stdin=True)
                stop(process)
                assert attached_id() == 0
                checks.append('closed_stdin_detach_and_cleanup')
                command('ip', 'link', 'add', 'xc', 'type', 'veth', 'peer', 'name', 'xd')
                command('ip', 'link', 'set', 'xc', 'up')
                command('ip', 'link', 'set', 'xd', 'up')
                process, ident = start(mode, device='xc')
                command('ip', 'link', 'delete', 'xc')
                stop(process)
                checks.append('device_removed_cleanup')
                results['modes'][mode] = {'status': 'PASS', 'checks': checks}
                print(f'{mode}: PASS ({len(checks)} checks)', flush=True)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except S.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
        print(json.dumps(results, indent=2), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
