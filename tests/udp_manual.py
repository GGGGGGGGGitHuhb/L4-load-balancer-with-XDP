#!/usr/bin/env python3
"""UDP 手动验证工具；仅 Python3 标准库，客户端在全部命令间保持同一 socket。"""
import argparse
import signal
import socket
import sys

MASKS = {"A": 0x55, "B": 0xAA}


def endpoint(text):
    host, port = text.rsplit(":", 1)
    socket.inet_pton(socket.AF_INET, host)
    value = int(port)
    if not 0 <= value <= 65535:
        raise ValueError("port out of range")
    return host, value


def show_address(value):
    return f"{value[0]}:{value[1]}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("backend", "client"))
    parser.add_argument("--bind", required=True, type=endpoint)
    parser.add_argument("--mark", choices=MASKS, default="A")
    parser.add_argument("--timeout", type=float, default=1.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    stopped = False

    def stop(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(args.bind)
        local = show_address(sock.getsockname())
        sock.settimeout(0.1 if args.mode == "backend" else args.timeout)
        print(f"{args.mode} ready {local} mark={args.mark}", flush=True)
        if args.mode == "backend":
            while not stopped:
                try:
                    payload, peer = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                response = bytes(byte ^ MASKS[args.mark] for byte in payload)
                if sock.sendto(response, peer) != len(response):
                    raise RuntimeError("backend short send")
                print(f"backend={args.mark} bytes={len(payload)} peer={show_address(peer)}", flush=True)
            print("backend stopped", flush=True)
        else:
            # send IPv4:port HEX|- A|B|none；quit。循环内不重新 bind/创建 socket。
            for line in sys.stdin:
                if stopped or line.strip() == "quit":
                    break
                words = line.split()
                if not words:
                    continue
                if len(words) != 4 or words[0] != "send" or words[3] not in (*MASKS, "none"):
                    raise ValueError("use: send IPv4:port HEX|- A|B|none, or quit")
                dest = endpoint(words[1])
                payload = b"" if words[2] == "-" else bytes.fromhex(words[2])
                if len(payload) > 65507:
                    raise ValueError("payload exceeds 65507")
                if sock.sendto(payload, dest) != len(payload):
                    raise RuntimeError("client short send")
                try:
                    response, peer = sock.recvfrom(65535)
                except socket.timeout:
                    if words[3] != "none":
                        raise RuntimeError("expected reply timed out")
                    print(f"PASS no-response client={local} target={show_address(dest)} bytes={len(payload)}", flush=True)
                    continue
                if words[3] == "none":
                    raise RuntimeError("unexpected reply for failed old packet")
                expected = bytes(byte ^ MASKS[words[3]] for byte in payload)
                if response != expected or peer != dest:
                    raise RuntimeError(f"reply bytes/source mismatch: {show_address(peer)}")
                print(f"PASS client={local} target={show_address(dest)} source={show_address(peer)} backend={words[3]} bytes={len(payload)}", flush=True)
            print("client stopped", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"FAIL {error}", file=sys.stderr, flush=True)
        sys.exit(1)
