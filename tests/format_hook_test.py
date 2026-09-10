#!/usr/bin/env python3
"""Exercise real pre-commit execution in a fresh disposable Git repository."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new evidence directory')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    repo = output / 'repository with spaces'
    repo.mkdir()
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
    calls = []

    def git(*argv, expected=0, input=None, environment=None):
        command = ['git', '-c', 'commit.gpgsign=false', '-C', str(repo), *argv]
        done = subprocess.run(command, input=input, capture_output=True, env=environment or env)
        if argv[0] == 'commit':
            calls.append({'argv': command, 'returncode': done.returncode, 'expected': expected,
                          'stdout': done.stdout.decode(), 'stderr': done.stderr.decode()})
            (output / 'commits.json').write_text(json.dumps(calls, indent=2, ensure_ascii=False) + '\n')
        assert done.returncode == expected, (argv, done.returncode, done.stderr.decode())
        return done.stdout

    git('init', '-b', 'main')
    git('config', 'user.name', 'Hook fixture')
    git('config', 'user.email', 'hook-fixture@example.invalid')
    git('config', 'core.hooksPath', '.githooks')
    (repo / '.githooks').mkdir()
    shutil.copy2(ROOT / '.githooks/pre-commit', repo / '.githooks/pre-commit')
    shutil.copy2(ROOT / '.clang-format', repo / '.clang-format')
    (repo / '.gitignore').write_text('/.stage-tmp/\n')
    names = ['source file.cpp', 'line\nbreak.h', '-leading.cpp']
    for name in names:
        (repo / name).write_text('int value(){return 1;}\n')
    git('add', '--', '.')
    before = git('write-tree')
    git('commit', '-m', 'must format first', expected=1)
    assert before == git('write-tree'), 'hook changed staged tree'
    assert all((repo / name).read_text() == 'int value() { return 1; }\n' for name in names)
    git('add', '--', '.')
    git('commit', '-m', 'formatted initial commit')

    # Every tracked source is checked, even if this commit stages no C++ file.
    (repo / names[2]).write_text('int value(){return 1;}\n')
    before = git('write-tree')
    git('commit', '--allow-empty', '-m', 'unstaged tracked formatting', expected=1)
    assert before == git('write-tree')
    assert (repo / names[2]).read_text() == 'int value() { return 1; }\n'

    # A formatted partially staged file commits only its staged logical value.
    path = repo / names[0]
    path.write_text('int value() { return 2; }\n')
    git('add', '--', names[0])
    path.write_text('int value() { return 3; }\n')
    git('commit', '-m', 'formatted partial stage')
    assert git('show', 'HEAD:' + names[0]) == b'int value() { return 2; }\n'
    assert 'return 3' in path.read_text()

    # An unformatted index stays untouched even when the worktree is formatted.
    path.write_text('int value(){return 4;}\n')
    git('add', '--', names[0])
    path.write_text('int value(){return 5;}\n')
    before = git('write-tree')
    git('commit', '-m', 'unformatted partial stage', expected=1)
    assert before == git('write-tree')
    assert path.read_text() == 'int value() { return 5; }\n'
    git('commit', '-m', 'worktree formatted index still bad', expected=1)
    assert before == git('write-tree')
    # Simulate the user's selective staging without adding the unstaged value 5.
    blob = git('hash-object', '-w', '--stdin', input=b'int value() { return 4; }\n').decode().strip()
    git('update-index', '--cacheinfo', '100644', blob, names[0])
    git('commit', '-m', 'selectively staged format')
    assert git('show', 'HEAD:' + names[0]) == b'int value() { return 4; }\n'
    assert 'return 5' in path.read_text()

    untracked = repo / 'untracked.cpp'
    untracked.write_text('int untouched(){return 9;}\n')
    untracked_before = untracked.read_bytes()
    (repo / names[1]).unlink()  # Unstaged deletion must not be resurrected.
    git('commit', '--allow-empty', '-m', 'unstaged deletion and untracked file')
    assert not (repo / names[1]).exists() and untracked.read_bytes() == untracked_before
    git('rm', '--', names[1])
    git('commit', '-m', 'staged deletion')
    assert not (repo / names[1]).exists()

    # Never follow tracked symlinks, including a .cpp link to an outside file.
    external = output / 'outside.cpp'
    external.write_text('int outside(){return 7;}\n')
    (repo / 'link.cpp').symlink_to(external)
    git('add', '--', 'link.cpp')
    git('commit', '-m', 'symlink is not source content')
    assert external.read_text() == 'int outside(){return 7;}\n'

    original_style = (repo / '.clang-format').read_bytes()
    (repo / '.clang-format').write_bytes(original_style + b'ColumnLimit: 90\n')
    before = git('write-tree')
    git('commit', '--allow-empty', '-m', 'unstaged style', expected=1)
    assert before == git('write-tree')
    (repo / '.clang-format').write_bytes(original_style)

    bindir = output / 'without formatter'
    bindir.mkdir()
    for command in ['bash', 'git']:
        (bindir / command).symlink_to(shutil.which(command))
    missing_env = dict(env, PATH=str(bindir))
    before = git('write-tree')
    git('commit', '--allow-empty', '-m', 'missing formatter', expected=1, environment=missing_env)
    assert before == git('write-tree') and '缺少 clang-format' in calls[-1]['stderr']
    assert not list((repo / '.stage-tmp/git-hooks').iterdir()), 'temporary files leaked'
    result = {'valid': True, 'real_commit_attempts': len(calls),
              'expected_rejections': sum(c['expected'] != 0 for c in calls),
              'index_protected': True, 'untracked_untouched': True,
              'spaces_newline_leading_dash': True, 'deletions_and_symlinks_safe': True,
              'hook_sha256': hashlib.sha256((ROOT / '.githooks/pre-commit').read_bytes()).hexdigest()}
    (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
