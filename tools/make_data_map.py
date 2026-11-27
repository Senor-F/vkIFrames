#!/usr/bin/env python3
# Writes index/data.md: size, type, purpose and section start lines of read-only data files (tests/data, lock files).
"""Usage: python3 tools/make_data_map.py   (run from anywhere; writes index/data.md in the repository root)

Data files are indexed, never edited. Files over LARGE_LINES lines also list where each section starts
(top-level YAML keys), so an agent can jump there with Read offset/limit.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LARGE_LINES = 500
# (path glob prefix/regex, type, purpose); first match wins.
RULES = [
    (r'^tests/data/ref_(\w+)\.txt$', 'text trace: line 1 "# target_fps kmax", then one line per native frame',
     'expected output of the C++ scheduler for the "{0}" frame-time case; made by tools/clock_sim/export_ref.py, read by tests/test_scheduler.cpp'),
    (r'^decky/pnpm-lock\.yaml$', 'YAML lock file (third-party, read-only)',
     'pinned versions of the Decky plugin build dependencies; changed only by pnpm'),
]


def tracked():
    out = subprocess.check_output(['git', '-C', ROOT, 'ls-files'], text=True)
    return out.split('\n')


def sections(path):
    starts = []
    with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            if re.match(r'^[A-Za-z_][\w-]*:', line) and path.endswith('.yaml'):
                starts.append((n, line.split(':')[0]))
    return starts


def main():
    rows = []
    for path in tracked():
        for pat, kind, purpose in RULES:
            m = re.match(pat, path)
            if m:
                full = os.path.join(ROOT, path)
                with open(full, 'rb') as f:
                    lines = f.read().count(b'\n')
                rows.append((path, os.path.getsize(full), lines, kind, purpose.format(*m.groups()), sections(path) if lines > LARGE_LINES else []))
                break
    out = ['# Data map (generated: edit tools/make_data_map.py instead)', '',
           'Read-only data files. `path | size | lines | type | purpose`. Large files list where sections start (line numbers), so jump there with offset/limit.', '']
    for path, size, lines, kind, purpose, secs in sorted(rows):
        out.append('%s | %d bytes | %d lines | %s | %s' % (path, size, lines, kind, purpose))
        if secs:
            out.append('  sections start at line: ' + ', '.join('%d %s' % s for s in secs))
    with open(os.path.join(ROOT, 'index', 'data.md'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('index/data.md: %d files' % len(rows))


if __name__ == '__main__':
    sys.exit(main())
