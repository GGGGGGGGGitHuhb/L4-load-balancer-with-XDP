"""Fixed Git-object product provenance. No worktree checkout or Git writes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
VERSIONS = {'baseline': ('v0.4-s1', '8a1b9393e8a1710152274be525c015bb2623625f'),
            'candidate': ('v0.4-s2', '48a12831b2d0a767fd5d4bb1b2899d2f078be2ac')}
FLAGS = {'CMAKE_BUILD_TYPE': 'Release', 'BUILD_TESTING': 'OFF',
         'CMAKE_CXX_FLAGS': '', 'CMAKE_EXE_LINKER_FLAGS': ''}


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)


def tools_identity():
    return {p.name: sha(p) for pattern in ('benchmark_*.py', 'v04_benchmark_*.py')
            for p in sorted((ROOT / 'tests').glob(pattern))}


def source_files(commit):
    result = {}
    for line in git('ls-tree', '-r', commit).decode().splitlines():
        meta, name = line.split('\t', 1)
        mode, kind, blob = meta.split()
        if name.startswith(('src/', 'configs/')) or name == 'CMakeLists.txt':
            require(kind == 'blob' and mode in ('100644', '100755'), 'unsupported source type')
            result[name] = hashlib.sha256(git('cat-file', 'blob', blob)).hexdigest()
    return result


def static_manifest(manifest):
    require(manifest['schema'] == 1, 'manifest schema')
    role = manifest['role']
    ref, commit = VERSIONS[role]
    require(manifest['ref'] == ref and manifest['commit'] == commit, 'manifest fixed ref mismatch')
    require(git('rev-parse', ref + '^{}').decode().strip() == commit, 'tag moved')
    require(manifest['tag_object'] == git('rev-parse', ref).decode().strip(), 'tag object mismatch')
    require(manifest['tree'] == git('rev-parse', commit + '^{tree}').decode().strip(), 'tree mismatch')
    require(manifest['sources'] == source_files(commit), 'Git source fingerprint mismatch')
    require(manifest['settings'] == FLAGS, 'build settings mismatch')
    require(all(c['returncode'] == 0 for c in manifest['commands']), 'failed build manifest')
    expected = [['cmake', '-S', manifest['source_path'], '-B', manifest['build_path'], '-G', 'Ninja',
                 '-DCMAKE_CXX_COMPILER=' + manifest['compiler_path'], *['-D' + k + '=' + v for k, v in FLAGS.items()]],
                ['cmake', '--build', manifest['build_path'], '-j4']]
    require([c['argv'] for c in manifest['commands']] == expected, 'build argv mismatch')
    require(manifest['effective_flags']['CMAKE_CXX_FLAGS_RELEASE'] == '-O3 -DNDEBUG', 'Release flags mismatch')
    require(len(manifest['binary_sha256']) == 64, 'binary digest missing')


def verify(manifest, program=None):
    static_manifest(manifest)
    source, build = Path(manifest['source_path']), Path(manifest['build_path'])
    for name, expected in manifest['sources'].items():
        require(sha(source / name) == expected, 'exported source changed: ' + name)
    require(sha(build / 'CMakeCache.txt') == manifest['cache_sha256'], 'build cache changed')
    require(cache_flags(build / 'CMakeCache.txt') == manifest['effective_flags'], 'effective cache flags changed')
    require(sha(program or manifest['binary_path']) == manifest['binary_sha256'], 'binary/manifest mismatch')
    return manifest


def cache_flags(path):
    return {line.split(':', 1)[0]: line.split('=', 1)[1]
            for line in path.read_text().splitlines()
            if line.startswith(('CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:', 'CMAKE_EXE_LINKER_FLAGS:',
                                'CMAKE_EXE_LINKER_FLAGS_RELEASE:', 'CMAKE_BUILD_TYPE:', 'BUILD_TESTING:'))}


def prepare(output):
    output.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which('clang++')
    require(compiler is not None, 'clang++ unavailable')
    versions = {name: subprocess.check_output([command, '--version'], text=True).strip()
                for name, command in [('compiler', compiler), ('cmake', 'cmake'), ('ninja', 'ninja')]}
    for role, (ref, commit) in VERSIONS.items():
        require(git('rev-parse', ref + '^{}').decode().strip() == commit, 'fixed tag mismatch')
        base = output / role
        source, build = base / 'source', base / 'build'
        source.mkdir(parents=True)
        # Export individual regular Git blobs: no tar traversal, symlinks or checkout.
        for line in git('ls-tree', '-r', commit).decode().splitlines():
            meta, name = line.split('\t', 1)
            mode, kind, blob = meta.split()
            require(kind == 'blob' and mode in ('100644', '100755'), 'unsupported Git archive member')
            require(not Path(name).is_absolute() and '..' not in Path(name).parts, 'unsafe source member')
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(git('cat-file', 'blob', blob))
            path.chmod(0o555 if mode == '100755' else 0o444)
        commands = []
        argv_list = [['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
                      '-DCMAKE_CXX_COMPILER=' + compiler, *['-D' + k + '=' + v for k, v in FLAGS.items()]],
                     ['cmake', '--build', str(build), '-j4']]
        for index, argv in enumerate(argv_list):
            with (base / f'build-{index}.log').open('w') as log:
                code = subprocess.call(argv, stdout=log, stderr=subprocess.STDOUT)
            commands.append({'argv': argv, 'returncode': code})
            write(base / 'commands.json', commands)
            require(code == 0, 'build failed: ' + role)
        binary = build / 'bin/l4lb'
        manifest = {'schema': 1, 'role': role, 'ref': ref, 'commit': commit,
                    'tag_object': git('rev-parse', ref).decode().strip(),
                    'tree': git('rev-parse', commit + '^{tree}').decode().strip(),
                    'sources': source_files(commit), 'settings': FLAGS, 'versions': versions,
                    'compiler_path': compiler, 'commands': commands,
                    'source_path': str(source), 'build_path': str(build), 'binary_path': str(binary),
                    'binary_sha256': sha(binary), 'cache_sha256': sha(build / 'CMakeCache.txt')}
        manifest['effective_flags'] = cache_flags(build / 'CMakeCache.txt')
        verify(manifest)
        write(base / 'manifest.json', manifest)
    return output
