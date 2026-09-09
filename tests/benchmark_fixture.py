"""Owned echo subprocess with explicit ready and shutdown evidence."""
import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--protocol', choices=['tcp', 'udp'], required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--fault', default='none')
    args = parser.parse_args()
    directory = args.directory
    baseline = len(os.listdir('/proc/self/fd'))
    done = False
    errors = []

    def stop(*unused):
        nonlocal done
        done = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    selector = selectors.DefaultSelector()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM if args.protocol == 'tcp' else socket.SOCK_DGRAM)
    listener.bind(('127.0.0.1', 0))
    listener.setblocking(False)
    if args.protocol == 'tcp':
        listener.listen(128)
    selector.register(listener, selectors.EVENT_READ, None)
    ready = directory / 'backend-ready.tmp'
    ready.write_text(json.dumps({'pid': os.getpid(), 'port': listener.getsockname()[1]}))
    ready.rename(directory / 'backend-ready.json')
    delayed = []
    cross_client_first = None
    altered_echoes = []
    counter = 0
    try:
        while not done:
            for key, events in selector.select(.01):
                sock = key.fileobj
                if sock is listener and args.protocol == 'tcp':
                    client, _ = listener.accept()
                    client.setblocking(False)
                    selector.register(client, selectors.EVENT_READ, bytearray())
                    continue
                if events & selectors.EVENT_READ:
                    if args.protocol == 'udp':
                        data, address = sock.recvfrom(65536)
                    else:
                        data = sock.recv(65536)
                        if not data:
                            selector.unregister(sock)
                            sock.close()
                            continue
                    counter += 1
                    if args.fault == 'background':
                        raise RuntimeError('injected backend event-loop exception')
                    if args.fault == 'corrupt':
                        data = bytes([data[0] ^ 1]) + data[1:]
                    if args.protocol == 'udp':
                        if args.fault == 'udp-cross-client' and counter <= 2:
                            if cross_client_first is None:
                                cross_client_first = (data, address)
                            else:
                                first_data, first_address = cross_client_first
                                # Rewrite client identity AND exchange destination
                                # sockets. Nonce/phase/seq and payload stay intact.
                                for original, recipient, destination in [
                                        (first_data, data, address),
                                        (data, first_data, first_address)]:
                                    assert original[20:24] != recipient[20:24]
                                    sock.sendto(original[:20] + recipient[20:24] + original[24:], destination)
                                    altered_echoes.append({'seq': int.from_bytes(original[24:32], 'big'),
                                                          'sent_client': int.from_bytes(original[20:24], 'big'),
                                                          'received_client': int.from_bytes(recipient[20:24], 'big')})
                        elif args.fault == 'udp-disorder':
                            # Deterministic duplicates, out-of-order responses, late responses.
                            delay = .15 if counter % 7 == 0 else (.015 if counter % 2 == 0 else 0)
                            delayed.append((time.monotonic() + delay, data, address))
                            if counter % 3 == 0:
                                delayed.append((time.monotonic() + .002, data, address))
                        else:
                            sock.sendto(data, address)
                    else:
                        key.data.extend(data)
                        selector.modify(sock, selectors.EVENT_READ | selectors.EVENT_WRITE, key.data)
                if events & selectors.EVENT_WRITE:
                    # Fragment responses explicitly in the partial-I/O test.
                    count = sock.send(key.data[:7] if args.fault == 'tcp-partial' else key.data)
                    del key.data[:count]
                    if not key.data:
                        selector.modify(sock, selectors.EVENT_READ, key.data)
            now = time.monotonic()
            for item in list(delayed):
                if item[0] <= now:
                    listener.sendto(item[1], item[2])
                    delayed.remove(item)
    except Exception as error:
        errors.append(str(error))
    finally:
        for key in list(selector.get_map().values()):
            key.fileobj.close()
        selector.close()
        final = len(os.listdir('/proc/self/fd'))
        (directory / 'backend-cleanup.json').write_text(json.dumps({
            'fd_before': baseline, 'fd_after': final, 'errors': errors, 'pid': os.getpid(),
            'altered_echoes': altered_echoes}))
    return 1 if errors or final != baseline else 0


if __name__ == '__main__':
    raise SystemExit(main())
