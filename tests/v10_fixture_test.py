"""Deterministic paired-port collision/error and cleanup regression for F-001."""
import errno
import os
import socket
import threading
from unittest.mock import patch

import v03_fixture as fixture


class Suite:
    fault = ''

    def __init__(self):
        self.actions = []

    def action(self, kind, **values):
        self.actions.append((kind, values))


def main():
    original = fixture.bound
    before = len(os.listdir('/proc/self/fd'))
    threads = {t.ident for t in threading.enumerate()}
    with original(socket.SOCK_STREAM) as occupied:
        occupied.listen(1)
        occupied_port = occupied.getsockname()[1]
        for mode in ['first-conflict', 'all-conflict', 'other-error']:
            candidates = []
            attempts = 0
            suite = Suite()
            error = OSError(errno.EACCES, 'injected permission error')
            case_fds = len(os.listdir('/proc/self/fd'))
            pair = fixture.bound_udp_tcp() if mode == 'first-conflict' else None

            def controlled(kind, port=0):
                nonlocal attempts
                if kind == socket.SOCK_DGRAM:
                    attempts += 1
                    # Real UDP bind succeeds at a TCP-occupied port. Subsequent
                    # success reserves both protocols until Backend takes them.
                    if mode != 'first-conflict' or attempts == 1:
                        value = original(kind, occupied_port)
                    else:
                        assert attempts == 2
                        value = pair[0]
                    candidates.append(value)
                    return value
                if mode == 'first-conflict' and attempts == 2:
                    assert port == pair[0].getsockname()[1]
                    return pair[1]
                if mode == 'other-error':
                    raise error
                return original(kind, port)

            with patch.object(fixture, 'bound', controlled):
                backend = None
                try:
                    backend = fixture.Backend(suite, 0, udp=True)
                    assert mode == 'first-conflict', 'unexpected constructor success'
                    assert attempts == 2, 'expected one collision then reserved pair success'
                    assert backend.listener.getsockname()[1] == backend.udp.getsockname()[1]
                    assert not backend.errors
                except OSError as caught:
                    if mode == 'all-conflict':
                        assert caught.errno == errno.EADDRINUSE and attempts == 16
                        assert 'exhausted after 16 attempts' in str(caught)
                    elif mode == 'other-error':
                        assert caught is error and attempts == 1, 'non-EADDRINUSE retried/replaced'
                    else:
                        raise
                finally:
                    if backend is not None:
                        backend.close()
                    if pair is not None:
                        for value in pair:
                            value.close()
            assert all(value.fileno() == -1 for value in candidates), 'UDP candidate leak'
            assert len(os.listdir('/proc/self/fd')) == case_fds, 'socket leak'
            assert {t.ident for t in threading.enumerate()} == threads, 'worker leak'
            cleanup = [values for kind, values in suite.actions if kind == 'backend-cleanup']
            assert len(cleanup) == 1
            assert cleanup[0]['threads'] == (2 if mode == 'first-conflict' else 0)
            print(f'PASS {mode}: attempts={attempts}, sockets/threads reclaimed')
    assert len(os.listdir('/proc/self/fd')) == before
    print('PASS paired allocation: all owned sockets/threads reclaimed')


if __name__ == '__main__':
    main()
