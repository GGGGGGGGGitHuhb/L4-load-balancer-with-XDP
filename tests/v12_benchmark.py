#!/usr/bin/env python3
"""V1.2 benchmark entry point. Build fixed S3, run, validate, recompute."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import signal
import subprocess as S
import sys
import time
import uuid

from v12_benchmark_data import SCHEMA, digest, encoded, load, matrix, require, save, sha, summarize
from v12_benchmark_measure import client, echo, system_sample

ROOT = Path(__file__).resolve().parents[1]
FIXED = '2dd2c98283773925fe179bc58d9e93ceac148ab7'


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + '\n')


def output(argv):
    return S.check_output(list(map(str, argv)), text=True).strip()


def build(args):
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    source = directory / 'source'
    source.mkdir()
    # Trusted local fixed tree; no untrusted archive or public-package extraction.
    archive = S.Popen(['git', '-C', str(ROOT), 'archive', FIXED], stdout=S.PIPE)
    S.run(['tar', '-x', '-C', str(source)], stdin=archive.stdout, check=True)
    archive.stdout.close()
    require(archive.wait() == 0, 'git archive')
    prefix = args.libbpf_prefix.resolve()
    library = prefix / 'lib/x86_64-linux-gnu/libbpf.so'
    commands = []
    for mode in ('on', 'off'):
        build_dir = directory / mode
        cmd = ['cmake', '-S', str(source), '-B', str(build_dir), '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
               '-DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON', '-DL4LB_BUILD_XDP=' + ('ON' if mode == 'on' else 'OFF'),
               '-DL4LB_BUILD_XDP_LOADER=' + ('ON' if mode == 'on' else 'OFF')]
        if mode == 'on':
            cmd += [f'-DL4LB_LIBBPF_INCLUDE_DIR={prefix / "include"}', f'-DL4LB_LIBBPF_LIBRARY={library}',
                    f'-DL4LB_BPF_INCLUDE_DIRS={prefix / "include"}']
        commands.append(cmd)
        with (directory / f'{mode}-build.log').open('w') as log:
            S.run(cmd, stdout=log, stderr=S.STDOUT, check=True)
            S.run(['cmake', '--build', str(build_dir), '-j', '4'], stdout=log, stderr=S.STDOUT, check=True)
    paths = {'proxy': directory / 'off/bin/l4lb', 'loader': directory / 'on/bin/l4lb-xdp',
             'pass': directory / 'on/xdp/xdp_pass.bpf.o', 'static': directory / 'on/xdp/xdp_udp_dsr.bpf.o',
             'runtime': directory / 'on/xdp/xdp_udp_runtime.bpf.o'}
    sources = output(['git', '-C', ROOT, 'ls-tree', '-r', '--name-only', FIXED]).splitlines()
    identity = dict(product=dict(commit=FIXED, tree=output(['git', '-C', ROOT, 'rev-parse', FIXED + '^{tree}']),
                                tag='v1.2-s3', tag_object=output(['git', '-C', ROOT, 'rev-parse', 'v1.2-s3']),
                                sources={p: sha(source / p) for p in sources if p.startswith(('src/', 'configs/', 'tests/'))}),
                    binaries={k: dict(path=str(p), sha256=sha(p)) for k, p in paths.items()}, libbpf=str(library),
                    libbpf_sha256=sha(library.resolve()), commands=commands,
                    compiler=output(['c++', '--version']).splitlines()[0], clang=output(['clang', '--version']).splitlines()[0],
                    cmake=output(['cmake', '--version']).splitlines()[0])
    write(directory / 'product.json', identity)
    print(json.dumps(dict(product=str(directory / 'product.json'), binaries={k: v['sha256'] for k, v in identity['binaries'].items()})))


def tools_identity():
    return {p.name: sha(p) for p in sorted((ROOT / 'tests').glob('v12_benchmark*.py'))}


def verify_identity(identity):
    require(identity['tools'] == tools_identity(), 'wrong tool identity')
    require(identity['product']['commit'] == FIXED, 'wrong product commit')
    for item in identity['binaries'].values():
        require(sha(item['path']) == item['sha256'], 'wrong binary identity')
    require(sha(Path(identity['libbpf']).resolve()) == identity['libbpf_sha256'], 'wrong library identity')


def environment():
    cpu = [line for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith(('model name', 'cpu MHz'))]
    cgroup = {}
    for name in ('cpu.max', 'memory.max', 'cpuset.cpus.effective'):
        path = Path('/sys/fs/cgroup') / name
        cgroup[name] = path.read_text().strip() if path.exists() else None
    return dict(utc=datetime.now(timezone.utc).isoformat(), kernel=platform.release(), python=sys.version,
                cpu=cpu, affinity=sorted(os.sched_getaffinity(0)), cgroup=cgroup,
                ip=output(['ip', '-Version']), ethtool=output(['ethtool', '--version']))


def internal(args):
    from v12_benchmark_topology import Topology
    directory = Path(args.directory)
    request = json.loads((directory / 'request.json').read_text())
    identity, spec = request['identity'], request['spec']
    verify_identity(identity)
    (directory / 'namespace-init.pid').write_text(str(os.getpid()))
    topology = Topology(directory, identity, spec, args.fault)
    result = dict(spec=spec, identity=digest(identity), valid=False, error=None, cleanup_errors=[], nonce=uuid.uuid4().hex,
                  ticks=os.sysconf('SC_CLK_TCK'), events=topology.events, resources=topology.resources, commands=topology.commands)
    def interrupted(sig, _frame):
        raise RuntimeError(f'signal termination {sig}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        topology.setup()
        result['network'] = topology.network
        result['preflight'], result['traffic'] = topology.measure(result['nonce'])
        result['valid'] = True
    except BaseException as exc:
        result['error'] = f'{type(exc).__name__}: {exc}'
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            result['cleanup'] = topology.close()
        except Exception as exc:
            topology.errors.append(str(exc))
        result['cleanup_errors'] = topology.errors
        result['backends'] = [json.loads((directory / f'echo-{i}.json').read_text()) if (directory / f'echo-{i}.json').exists() else {} for i in range(2)]
        write(directory / 'run.json', result)
    if result['error'] or result['cleanup_errors']:
        print(result['error'], result['cleanup_errors'])
        return 1
    return 0


def run_one(directory, identity, spec, fault='', watchdog=45):
    directory.mkdir()
    write(directory / 'request.json', dict(identity=identity, spec=spec))
    cmd = ['unshare', '--mount', '--net', '--pid', '--fork', '--mount-proc', sys.executable, str(Path(__file__).resolve()), '_run', str(directory), '--fault', fault]
    process = None
    error = None
    start = time.monotonic()
    with (directory / 'supervisor.log').open('w') as log:
        try:
            process = S.Popen(cmd, stdout=log, stderr=log, start_new_session=True)
            code = process.wait(timeout=watchdog)
            if code:
                error = f'run exited {code}'
        except S.TimeoutExpired:
            error = 'watchdog deadline'
        except BaseException as exc:
            error = f'outer interruption: {exc}'
        finally:
            if process and process.poll() is None:
                # PID namespace init receives TERM, performs finally; KILL fallback
                # destroys its owned PID namespace, including uncooperative descendants.
                children = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
                for pid in children:
                    try:
                        os.kill(int(pid), signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    process.wait(timeout=5)
                except S.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
    remaining = []
    if process:
        for path in Path('/proc').glob('[0-9]*/stat'):
            try:
                fields = path.read_text().rsplit(')', 1)[1].split()
                if int(fields[2]) == process.pid and fields[0] != 'Z':
                    remaining.append(int(path.parent.name))
            except (FileNotFoundError, ProcessLookupError):
                pass
    if remaining:
        error = (error or '') + ' remaining owned process group'
    write(directory / 'supervisor.json', dict(remaining_group=remaining, pgid=process.pid if process else None, error=error, returncode=process.returncode if process else None,
                                             elapsed_s=time.monotonic() - start, owned_reaped=process is not None and process.poll() is not None))
    if error:
        raise RuntimeError(error + ': ' + (directory / 'supervisor.log').read_text()[-2000:])
    return json.loads((directory / 'run.json').read_text())


def run(args):
    require(os.geteuid() == 0, 'run requires root for owned namespaces')
    def stop(sig, _frame):
        raise RuntimeError(f'outer signal termination {sig}')
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    directory = args.directory.resolve()
    require(not directory.exists(), 'output directory already exists')
    identity = json.loads(args.product.read_text())
    identity['tools'] = tools_identity()
    if args.fault == 'identity':
        identity['binaries']['proxy']['sha256'] = '0' * 64
    verify_identity(identity)
    directory.mkdir(parents=True)
    formal = not args.short
    specs = matrix()
    if args.short:
        specs = [dict(path=p, payload=args.short_payload, rate=args.short_rate, repeat=0, scene='steady', kind='data', run_id=f'{i:03d}', clients=16,
                      warmup_s=args.short_warmup, window_s=args.short_window, timeout_s=1) for i, p in enumerate(args.paths.split(','))]
        if args.scene != 'steady':
            specs = [dict(specs[0], scene=args.scene, kind='control', window_s=6, payload=256)]
    package = dict(schema_version=SCHEMA, formal=formal, identity=identity, manifest=specs, environment=environment(), runs=[])
    package['idle'] = [system_sample()]
    time.sleep(3)
    package['idle'].append(system_sample())
    write(directory / 'manifest.json', package)
    for spec in specs:
        result = run_one(directory / spec['run_id'], identity, spec, args.fault, args.watchdog)
        from v12_benchmark_data import validate_run
        validate_run(result, identity)
        package['runs'].append(dict(sha256=digest(result), data=result))
        print(json.dumps(dict(completed=len(package['runs']), total=len(specs), path=spec['path'], counts=result['traffic']['counts'])), flush=True)
    summary = summarize(package)
    save(package, directory / 'result.raw.json.gz')
    write(directory / 'result.json', summary)
    (directory / 'result.raw.sha256').write_text(sha(directory / 'result.raw.json.gz') + '  result.raw.json.gz\n')


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '_echo':
        echo(sys.argv[2], int(sys.argv[3]), sys.argv[4])
        return 0
    if len(sys.argv) > 1 and sys.argv[1] == '_client':
        client(sys.argv[2], json.loads(sys.argv[3]), sys.argv[4])
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build')
    b.add_argument('--directory', type=Path, required=True)
    b.add_argument('--libbpf-prefix', type=Path, required=True)
    r = sub.add_parser('run')
    r.add_argument('--directory', type=Path, required=True)
    r.add_argument('--product', type=Path, required=True)
    r.add_argument('--short', action='store_true')
    r.add_argument('--paths', default=','.join(['direct', 'proxy', 'static-generic', 'static-native', 'runtime-generic', 'runtime-native']))
    r.add_argument('--short-rate', type=int, default=1000)
    r.add_argument('--short-payload', type=int, default=64)
    r.add_argument('--short-window', type=float, default=1)
    r.add_argument('--short-warmup', type=float, default=.2)
    r.add_argument('--scene', choices=['steady', 'unchanged', 'reorder'], default='steady')
    r.add_argument('--fault', choices=['', 'corrupt', 'cross-client', 'identity', 'child-exit', 'sampler', 'write', 'watchdog', 'sigterm'], default='')
    r.add_argument('--watchdog', type=float, default=45)
    i = sub.add_parser('_run')
    i.add_argument('directory')
    i.add_argument('--fault', default='')
    for name in ('validate', 'recompute'):
        p = sub.add_parser(name)
        p.add_argument('package', type=Path)
        p.add_argument('--summary', type=Path)
    args = parser.parse_args()
    if args.command == 'build':
        build(args)
    elif args.command == 'run':
        run(args)
    elif args.command == '_run':
        return internal(args)
    else:
        result = summarize(load(args.package))
        if args.summary:
            if args.command == 'validate':
                require(result == json.loads(args.summary.read_text()), 'summary mismatch')
            else:
                write(args.summary, result)
        print(json.dumps(dict(valid=True, formal=result['formal'], runs=len(result['runs']), package_sha256=sha(args.package))))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(f'{type(exc).__name__}: {exc}', file=sys.stderr)
        sys.exit(1)
