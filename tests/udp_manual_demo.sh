#!/usr/bin/env bash
# 单 shell 手动流程：使用公开工具、固定客户端 socket、只管理本脚本子进程。
set -eu
program=$(realpath "${1:?usage: bash tests/udp_manual_demo.sh path/to/l4lb output-root [base-port]}")
output=${2:?output root required}
base=${3:-18080}
script_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$output"
run_dir=$(mktemp -d "$output/manual-XXXXXX")
echo "evidence=$run_dir"
a_pid= b_pid= proxy_pid= one_pid= two_pid= three_pid=
wait_owned() {
  local pid=$1 done=0
  for _ in $(seq 1 150); do
    if ! kill -0 "$pid" 2>/dev/null; then done=1; break; fi
    sleep 0.02
  done
  if [ "$done" -eq 0 ]; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "FAIL owned child exit deadline pid=$pid" >&2
    return 1
  fi
  wait "$pid"
}
cleanup() {
  local rc=$? pid
  trap - EXIT INT TERM
  exec 3>&- 4>&- 5>&-
  for pid in "$one_pid" "$two_pid" "$three_pid" "$a_pid" "$b_pid" "$proxy_pid"; do
    if [ -n "$pid" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      if wait_owned "$pid"; then echo "cleanup reaped=$pid code=0"; else rc=1; echo "cleanup failed pid=$pid"; fi
    fi
  done
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
wait_for() {
  local file=$1 pattern=$2
  for _ in $(seq 1 150); do
    if rg -q "$pattern" "$file"; then return 0; fi
    sleep 0.02
  done
  echo "FAIL deadline file=$file pattern=$pattern" >&2
  return 1
}
start_a() {
  python3 "$script_dir/udp_manual.py" backend --bind "127.0.0.1:$((base+1))" --mark A > "$run_dir/a-$1.log" 2>&1 &
  a_pid=$!;echo "backend A pid=$a_pid port=$((base+1))"
  wait_for "$run_dir/a-$1.log" '^backend ready '
}
start_a initial
python3 "$script_dir/udp_manual.py" backend --bind "127.0.0.1:$((base+2))" --mark B > "$run_dir/b.log" 2>&1 &
b_pid=$!;echo "backend B pid=$b_pid port=$((base+2))"
wait_for "$run_dir/b.log" '^backend ready '
cat > "$run_dir/udp.conf" <<CONFIG
protocol=udp
listen=0.0.0.0:$base
backend=127.0.0.1:$((base+1))
backend=127.0.0.1:$((base+2))
CONFIG
"$program" --check-config "$run_dir/udp.conf" > "$run_dir/check.out" 2> "$run_dir/check.err"
"$program" --run "$run_dir/udp.conf" > "$run_dir/proxy.out" 2> "$run_dir/proxy.err" &
proxy_pid=$!;echo "proxy pid=$proxy_pid port=$base"
wait_for "$run_dir/proxy.out" "^UDP 服务已启动：0.0.0.0:$base$"
mkfifo "$run_dir/one.in" "$run_dir/two.in" "$run_dir/three.in"
exec 3<>"$run_dir/one.in" 4<>"$run_dir/two.in" 5<>"$run_dir/three.in"
python3 "$script_dir/udp_manual.py" client --bind "127.0.0.1:$((base+3))" < "$run_dir/one.in" > "$run_dir/one.log" 2>&1 3>&- 4>&- 5>&- &
one_pid=$!;echo "client1 pid=$one_pid port=$((base+3))"
python3 "$script_dir/udp_manual.py" client --bind "127.0.0.1:$((base+4))" < "$run_dir/two.in" > "$run_dir/two.log" 2>&1 3>&- 4>&- 5>&- &
two_pid=$!;echo "client2 pid=$two_pid port=$((base+4))"
wait_for "$run_dir/one.log" '^client ready '
wait_for "$run_dir/two.log" '^client ready '
printf 'send 127.0.0.1:%s 503100ff41 A\n' "$base" >&3
wait_for "$run_dir/one.log" 'source=127.0.0.1:.*backend=A bytes=5'
printf 'send 127.0.0.2:%s 503200ff42 B\n' "$base" >&3
wait_for "$run_dir/one.log" 'source=127.0.0.2:.*backend=B bytes=5'
printf 'send 127.0.0.1:%s - A\n' "$base" >&3
wait_for "$run_dir/one.log" 'backend=A bytes=0'
printf 'send 127.0.0.1:%s 7365636f6e64 A\n' "$base" >&4
wait_for "$run_dir/two.log" 'backend=A bytes=6'
# 第一 flow 的确切 id 是本产品日志的首次 created，不猜测等待足够久即已关闭。
flow_id=$(sed -n 's/^flow=\([0-9]*\).*reason=created.*/\1/p' "$run_dir/proxy.err" | head -1)
kill -TERM "$a_pid"
wait_owned "$a_pid"
echo "stopped A pid=$a_pid code=0"
a_pid=
printf 'send 127.0.0.1:%s 6661696c6564 none\n' "$base" >&3
wait_for "$run_dir/one.log" '^PASS no-response '
wait_for "$run_dir/proxy.err" "^flow=$flow_id .*errno=111"
printf 'send 127.0.0.1:%s 6e65772d42 B\n' "$base" >&3
wait_for "$run_dir/one.log" 'source=127.0.0.1:.*backend=B bytes=5'
start_a restored
python3 "$script_dir/udp_manual.py" client --bind "127.0.0.1:$((base+5))" < "$run_dir/three.in" > "$run_dir/three.log" 2>&1 3>&- 4>&- 5>&- &
three_pid=$!;echo "client3 pid=$three_pid port=$((base+5))"
wait_for "$run_dir/three.log" '^client ready '
printf 'send 127.0.0.1:%s 726573746f726564 A\n' "$base" >&5
wait_for "$run_dir/three.log" 'backend=A bytes=8'
printf 'send 127.0.0.1:%s 737461792d42 B\n' "$base" >&3
wait_for "$run_dir/one.log" 'backend=B bytes=6'
# 活跃代理占用 UDP 端口，第二次运行必须退出1且无ready。
if "$program" --run "$run_dir/udp.conf" > "$run_dir/occupied.out" 2> "$run_dir/occupied.err"; then
  echo 'FAIL occupied port unexpectedly started' >&2;exit 1
else
  rc=$?;test "$rc" -eq 1;test ! -s "$run_dir/occupied.out";echo 'occupied exit=1 no ready'
fi
printf 'quit\n' >&3;printf 'quit\n' >&4;printf 'quit\n' >&5
wait_owned "$one_pid";echo "client reaped=$one_pid code=0";one_pid=
wait_owned "$two_pid";echo "client reaped=$two_pid code=0";two_pid=
wait_owned "$three_pid";echo "client reaped=$three_pid code=0";three_pid=
kill -INT "$proxy_pid";wait_owned "$proxy_pid";echo "SIGINT proxy reaped=$proxy_pid code=0";proxy_pid=
kill -TERM "$a_pid" "$b_pid";wait_owned "$a_pid";wait_owned "$b_pid"
echo "backends reaped=$a_pid,$b_pid code=0";a_pid=;b_pid=
python3 - "$base" <<'PY'
import socket, sys
for port in range(int(sys.argv[1]), int(sys.argv[1])+6):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', port))
print('PASS all six owned ports rebind')
PY
echo 'PASS manual wildcard/binary/zero/stopped-A/new-B/restored-A/stable-B/cleanup'
