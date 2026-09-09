"""Whitelisted identity and Linux /proc measurement-window resource evidence."""
import hashlib
import os
from pathlib import Path
import platform
import resource
import subprocess
import threading
import time


def capture(call):
    try:
        return {'value': call(), 'reason': None}
    except Exception as error:
        return {'value': None, 'reason': str(error)}


def command(argv, cwd=None):
    return subprocess.check_output(argv, text=True, cwd=cwd, stderr=subprocess.DEVNULL, timeout=5).strip()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def environment(args, directory):
    repo = Path(__file__).resolve().parent.parent
    cache = args.program.parent.parent / 'CMakeCache.txt'
    build = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(('CMAKE_BUILD_TYPE:', 'CMAKE_CXX_COMPILER:', 'CMAKE_GENERATOR:')):
                key, value = line.split('=', 1)
                build[key.split(':')[0]] = value
    compiler = build.get('CMAKE_CXX_COMPILER', 'c++')
    return {
        'tool_version': 1, 'command': __import__('sys').argv,
        'parameters': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        'git_commit': capture(lambda: command(['git', 'rev-parse', 'HEAD'], repo)),
        'git_status': capture(lambda: command(['git', 'status', '--porcelain', '--untracked-files=normal'], repo)),
        'binary_sha256': capture(lambda: sha(args.program)),
        'tool_sha256': {p.name: sha(p) for p in sorted(repo.glob('tests/benchmark_*.py'))},
        'production_source_sha256': {str(p.relative_to(repo)): sha(p)
                                     for p in sorted((repo / 'src').rglob('*')) if p.is_file()},
        'build': build or {'value': None, 'reason': 'CMakeCache.txt not adjacent to binary build root'},
        'compiler': capture(lambda: command([compiler, '--version'])),
        'cmake': capture(lambda: command(['cmake', '--version'])),
        'kernel': platform.release(), 'platform': platform.platform(),
        'wsl': 'microsoft' in platform.release().lower(),
        'cpu_model': capture(lambda: next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name'))),
        'logical_cpus': os.cpu_count(), 'affinity': capture(lambda: sorted(os.sched_getaffinity(0))),
        'memory': capture(lambda: '\n'.join(line for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith(('MemTotal:', 'MemAvailable:')))),
        'fd_limit': list(resource.getrlimit(resource.RLIMIT_NOFILE)),
        'cgroup_membership': capture(lambda: Path('/proc/self/cgroup').read_text()),
        'cgroup_limits': {name: capture(lambda name=name: (Path('/sys/fs/cgroup') / name).read_text().strip()) for name in ('cpu.max', 'memory.max', 'pids.max')},
        'network_namespace': capture(lambda: os.readlink('/proc/self/ns/net')),
        'topology': 'local loopback; direct backend or l4lb -> same-class echo backend',
        'addresses': {'host': '127.0.0.1'}, 'directory': str(directory)}


def proc_sample(pid):
    text = Path(f'/proc/{pid}/stat').read_text()
    fields = text[text.rfind(')') + 2:].split()
    ticks = os.sysconf('SC_CLK_TCK')
    return {'monotonic_ns': time.monotonic_ns(),
            'cpu_seconds': (int(fields[11]) + int(fields[12])) / ticks,
            'rss_bytes': int(fields[21]) * os.sysconf('SC_PAGE_SIZE'),
            'fd_count': len(os.listdir(f'/proc/{pid}/fd'))}


class Resources:
    def __init__(self, pids, check):
        self.pids = pids
        self.check = check
        self.rows = {name: [] for name in pids}
        self.errors = []
        self.done = threading.Event()
        self.thread = None

    def sample(self):
        self.check()
        for name, pid in self.pids.items():
            self.rows[name].append(proc_sample(pid))

    def start(self):
        self.sample()

        def loop():
            try:
                while not self.done.wait(.05):
                    self.sample()
            except Exception as error:
                self.errors.append(str(error))

        self.thread = threading.Thread(target=loop)
        self.thread.start()

    def stop(self):
        self.done.set()
        if self.thread:
            self.thread.join(2)
            if self.thread.is_alive():
                self.errors.append('resource sampler join failed')
        try:
            self.sample()
        except Exception as error:
            self.errors.append(str(error))
        if self.errors:
            raise RuntimeError('resource collection failed: ' + '; '.join(self.errors))
        result = {'product': {'value': None, 'reason': 'not applicable: direct mode'}}
        for name, rows in self.rows.items():
            first, last = rows[0], rows[-1]
            elapsed = (last['monotonic_ns'] - first['monotonic_ns']) / 1e9
            result[name] = {'pid': self.pids[name], 'start_ns': first['monotonic_ns'],
                            'end_ns': last['monotonic_ns'], 'wall_seconds': elapsed,
                            'cpu_seconds': last['cpu_seconds'] - first['cpu_seconds'],
                            'cpu_percent_one_core': 100 * (last['cpu_seconds'] - first['cpu_seconds']) / elapsed,
                            'sampled_peak_rss_bytes': max(r['rss_bytes'] for r in rows),
                            'sampled_peak_fd': max(r['fd_count'] for r in rows),
                            'sampling_period_seconds': .05, 'sample_count': len(rows)}
        return result
