#!/usr/bin/env python3
# Docs and index check: documents under 150 lines, links resolve, INDEX.md matches the tree. Exit 1 on any failure.
"""Usage: python3 tools/check_docs.py [ROOT]     (ROOT defaults to the repository root)

Fails on: a document (*.md, *.txt notes; not LICENSE/NOTICE, tests/data, third-party) of 150 lines or more;
a relative link in a document to a missing file; an indexed path (INDEX.md, index/*.md) that is missing;
a tracked or new (not ignored) file covered by no index line or folder line; a third-party, generated or
data folder with files but no index line. Outside a git tree it prints a note and passes.
"""
import fnmatch
import os
import re
import subprocess
import sys

DOC_MAX = 150
ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPECIAL_DIRS = {'third_party', 'third-party', 'vendor', 'external', 'node_modules', 'generated', 'data'}
NOT_DOCS = ('tests/data/',)
NOT_DOC_NAMES = {'LICENSE', 'NOTICE', 'CMakeLists.txt'}
TOKEN = re.compile(r'^[\w./*+-]+$')
LINK = re.compile(r'\]\(([^)\s]+)\)')


def files():
    try:
        out = subprocess.check_output(['git', '-C', ROOT, 'ls-files', '--cached', '--others', '--exclude-standard'],
                                      text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, OSError):
        return None
    return sorted(f for f in set(out.split('\n')) if f and os.path.isfile(os.path.join(ROOT, f)))


def is_doc(path):
    if os.path.basename(path) in NOT_DOC_NAMES or path.startswith(NOT_DOCS):
        return False
    if set(path.split('/')) & {'third_party', 'third-party', 'vendor', 'external', 'node_modules'}:
        return False
    return path.endswith(('.md', '.txt'))


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as f:
        return f.read().split('\n')


def check_size_and_links(docs, errs):
    for d in docs:
        lines = read(d)
        n = len(lines) - (1 if lines and lines[-1] == '' else 0)
        if n >= DOC_MAX:
            errs.append('%s: %d lines (a document stays under %d; split it, see CLAUDE.md Rule 1)' % (d, n, DOC_MAX))
        if not d.endswith('.md'):
            continue
        fence = False
        for i, line in enumerate(lines, 1):
            if line.lstrip().startswith('```'):
                fence = not fence
            if fence:
                continue
            for target in LINK.findall(line):
                t = target.split('#')[0]
                if not t or re.match(r'^[a-z][a-z0-9+.-]*:', t):
                    continue
                if not os.path.exists(os.path.normpath(os.path.join(ROOT, os.path.dirname(d), t))):
                    errs.append('%s:%d: broken link to %s' % (d, i, target))


def index_entries():
    """Returns (list of (file, line number, [patterns], generated flag))."""
    paths = ['INDEX.md'] + sorted('index/' + f for f in os.listdir(os.path.join(ROOT, 'index'))
                                  if f.endswith('.md')) if os.path.isdir(os.path.join(ROOT, 'index')) else ['INDEX.md']
    entries = []
    for p in paths:
        if not os.path.exists(os.path.join(ROOT, p)):
            continue
        for i, line in enumerate(read(p), 1):
            if ' | ' not in line or line.startswith(('Format', ' ')):
                continue
            first = line.split(' | ')[0]
            toks = []
            for part in first.split(', '):
                toks += [t.strip() for t in part.split(' ... ')]
            if toks and all(TOKEN.match(t) for t in toks):
                entries.append((p, i, toks, 'generated:' in line or 'git-ignored' in line))
    return entries


def covered(f, toks):
    return any(f == t or fnmatch.fnmatch(f, t) or (t.endswith('/') and f.startswith(t)) for t in toks)


def check_index(fl, errs):
    entries = index_entries()
    for p, i, toks, ignored in entries:
        if ignored:
            continue
        for t in toks:
            if not any(covered(f, [t]) for f in fl) and not os.path.exists(os.path.join(ROOT, t)):
                errs.append('%s:%d: indexed path %s does not exist' % (p, i, t))
    alltoks = [t for _, _, toks, _ in entries for t in toks]
    for f in fl:
        if not covered(f, alltoks):
            errs.append('%s: not covered by any line in INDEX.md or index/' % f)
    dirs = {}
    for f in fl:
        parts = f.split('/')
        for k in range(1, len(parts)):
            if parts[k - 1] in SPECIAL_DIRS:
                dirs['/'.join(parts[:k])] = 1
    for d in sorted(dirs):
        if not any(t == d + '/' or t.startswith(d + '/') for t in alltoks):
            errs.append('%s/: third-party, generated or data folder with no index line' % d)


def main():
    fl = files()
    if fl is None:
        print('check_docs: not a git tree, skipped')
        return 0
    if 'INDEX.md' not in fl:
        print('check_docs: no INDEX.md, skipped')
        return 0
    errs = []
    check_size_and_links([f for f in fl if is_doc(f)], errs)
    check_index(fl, errs)
    for e in errs:
        print('check_docs: ' + e)
    print('check_docs: %s (%d files, %d errors)' % ('FAIL' if errs else 'ok', len(fl), len(errs)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
