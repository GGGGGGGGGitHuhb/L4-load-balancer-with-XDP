#!/usr/bin/env python3
"""Check this repository's public Markdown file links and heading anchors offline."""
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT_DOCS = ('README.md', 'ARCHITECTURE.md', 'ROADMAP.md', 'CHANGELOG.md', 'TECH-DEBT-TRACKER.md')
PRIVATE = {'leader', 'builder', 'reviewer', 'references'}


def visible(text):
    lines = []
    fence = None
    for line in text.splitlines():
        match = re.match(r'^\s*(`{3,}|~{3,})', line)
        if match:
            mark = match[1][0]
            if fence is None:
                fence = mark
            elif fence == mark:
                fence = None
            continue
        if fence is None:
            lines.append(line)
    return '\n'.join(lines)


def anchors(text):
    seen = {}
    result = set()
    for line in visible(text).splitlines():
        match = re.match(r'^#{1,6}\s+(.+?)(?:\s+#+)?$', line)
        if not match:
            continue
        title = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', match[1]).replace('`', '')
        slug = re.sub(r'[^\w\-\s]', '', title.lower()).replace(' ', '-')
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        result.add(slug + (f'-{count}' if count else ''))
    return result


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    documents = [root / name for name in ROOT_DOCS]
    documents += sorted(p for p in (root / 'docs').rglob('*.md')
                        if not PRIVATE.intersection(p.relative_to(root / 'docs').parts))
    failures = []
    checked = 0
    for source in documents:
        if not source.is_file():
            failures.append(f'{source.relative_to(root)}: missing public document')
            continue
        text = re.sub(r'`[^`\n]*`', '', visible(source.read_text()))
        for match in re.finditer(r'!?\[[^]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+"[^"]*")?\)', text):
            url = match[1].strip('<>')
            parts = urlsplit(url)
            if parts.scheme or parts.netloc:
                continue
            checked += 1
            target = (source.parent / unquote(parts.path)).resolve() if parts.path else source
            reason = None
            if not target.is_relative_to(root):
                reason = 'outside public root'
            elif target.relative_to(root).parts[:1] == ('docs',) and PRIVATE.intersection(target.relative_to(root).parts[1:]):
                reason = 'private governance target'
            elif not target.exists():
                reason = 'missing target'
            elif parts.fragment and target.suffix == '.md' and unquote(parts.fragment) not in anchors(target.read_text()):
                reason = 'missing heading anchor'
            if reason:
                failures.append(f'{source.relative_to(root)}: {url}: {reason}')
    for failure in failures:
        print(failure)
    print(f'public_documents={len(documents)} local_links={checked} failures={len(failures)}')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
