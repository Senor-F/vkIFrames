#!/usr/bin/env python3
# Code size check: files over 500 lines, functions over 40 lines, and change size since a base ref.
"""Usage: code_size.py [--base REF] [--root DIR]

Measures tracked source files (C++, Python, shell, TypeScript, GLSL). Python functions come from the
syntax tree; the others from braces, after comments, string contents and preprocessor lines are blanked.
--base REF adds the change size since REF (added + removed lines, git diff --numstat REF).
Prints one line per violation, worst first, then a summary. Exit 1 when anything is over the limit.
"""
import argparse
import ast
import os
import re
import subprocess
import sys

FILE_MAX = 500
FUNC_MAX = 40
CHANGE_MAX = 400

LANG_BY_EXT = {
    '.cpp': 'c', '.hpp': 'c', '.h': 'c', '.comp': 'c', '.glsl': 'c', '.frag': 'c', '.vert': 'c',
    '.py': 'py', '.sh': 'sh', '.ts': 'ts', '.tsx': 'ts',
}
# Tracked files of these kinds are not code and are never reported as unmeasured.
QUIET_EXT = {'.md', '.txt', '.json', '.yaml', '.yml', '.in', '.template', '.map', '.supp',
             '.png', '.jpg', '.svg', '.ico', '.wav', '.ogg'}
QUIET_NAMES = {'LICENSE', 'NOTICE'}
THIRD_PARTY_PARTS = {'third_party', 'third-party', 'vendor', 'external', 'node_modules'}

KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'alignof', 'decltype',
            'static_assert', 'requires', '__attribute__', '__declspec', 'noexcept', 'throw',
            'new', 'delete', 'synchronized'}
NOT_FUNC_WORDS = {'class', 'struct', 'union', 'enum', 'namespace', 'extern', 'case', 'default',
                  'else', 'do', 'try', 'return', 'typedef', 'using'}
TS_NOT_FUNC = {'if', 'for', 'while', 'switch', 'catch', 'with', 'return'}

C_TOKEN = re.compile(r'''
    /\*.*?\*/                                    # block comment
  | //[^\n]*                                     # line comment
  | R"(?P<d>[^ ()\\\t\n]{0,16})\(.*?\)(?P=d)"   # raw string
  | "(?:\\.|[^"\\\n])*"?                         # string
  | (?<![0-9A-Za-z_])'(?:\\.|[^'\\\n])*'?        # char literal (not a digit separator)
  | ^[ \t]*\#(?:\\\n|[^\n])*                     # preprocessor line, with continuations
''', re.S | re.M | re.X)
TS_TOKEN = re.compile(r'''
    /\*.*?\*/ | //[^\n]* | `(?:\\.|[^`\\])*`? | "(?:\\.|[^"\\\n])*"? | '(?:\\.|[^'\\\n])*'?
''', re.S | re.X)
SH_TOKEN = re.compile(r'''
    \\[\s\S]                                     # escaped character
  | (?<![^\s;|&()<>])\#[^\n]*                    # comment: # starting a word
  | '[^'\n]*'?                                   # single-quoted
  | "(?:\\[\s\S]|[^"\\])*"?                      # double-quoted
''', re.X)
TOKENS = {'c': C_TOKEN, 'ts': TS_TOKEN, 'sh': SH_TOKEN}
HEREDOC = re.compile(r'(?<!<)<<-?\s*([\'"]?)(\w+)\1(?!<)')
SH_FN = re.compile(r'(?:function\s+)?([A-Za-z_][\w.:-]*)\s*\(\s*\)|function\s+([A-Za-z_][\w.:-]*)')
NAME_RE = re.compile(r'(operator\s*(?:\(\)|\[\]|[^\s\w(]+)'
                     r'|~?[A-Za-z_]\w*(?:\s*<[^;{}()]*>)?(?:\s*::\s*~?\w+(?:\s*<[^;{}()]*>)?)*)\s*$')
TRAILER = re.compile(r'\s*(?:->[^()]*|noexcept\s*\([^()]*\)|\b(?:const|noexcept|override|final|'
                     r'volatile|mutable)\b|&&?)\s*$')
TS_FUNC = re.compile(r'(?:export\s+)?(?:default\s+)?(?:async\s+)?function\b\s*\*?\s*([\w$]*)')
ARROW_NAME = re.compile(r'([\w$]+)\s*(?::[^=]*)?=\s*(?:async\s*)?[^=]*=>$')
TS_MODIFIERS = re.compile(r'^(?:\s*(?:public|private|protected|static|async|override|readonly|abstract)\b)*')


def git(root, *args):
    """Stdout of a read-only git command run in root (optional locks off, so a busy index is never written)."""
    env = dict(os.environ, GIT_OPTIONAL_LOCKS='0')
    res = subprocess.run(['git', '-C', root, *args], capture_output=True, env=env)
    if res.returncode:
        sys.exit('git ' + ' '.join(args) + ' failed: ' + res.stderr.decode('utf-8', 'replace').strip())
    return res.stdout.decode('utf-8', 'replace')


def skip_reason(path):
    """Why a tracked path is not measured, or '' when it is measured."""
    parts = path.split('/')
    if path.startswith('tests/data/'):
        return 'test data'
    if THIRD_PARTY_PARTS & set(parts[:-1]):
        return 'third-party'
    if re.search(r'(^|[-_.])lock(\.|$)', parts[-1]):
        return 'lock file'
    if re.search(r'generated', path, re.I):
        return 'generated'
    return ''


def blank(s):
    """Spaces in place of every character except newlines (keeps line numbers and columns)."""
    return re.sub(r'[^\n]', ' ', s)


def _keep_quotes(m):
    """Blank a literal's or comment's text but keep its quote marks, so 'extern "C"' still reads as a header."""
    s = m.group(0)
    if s[0] not in '"\'`':
        return blank(s)
    closed = len(s) > 1 and s[-1] == s[0]
    body_end = len(s) - 1 if closed else len(s)
    return s[0] + blank(s[1:body_end]) + (s[-1] if closed else '')


def _blank_heredocs(text):
    """Blank the bodies of shell here-documents (a line equal to the delimiter ends one)."""
    out, end = [], None
    for line in text.split('\n'):
        if end is not None:
            out.append(blank(line))
            if line.strip() == end:
                end = None
            continue
        out.append(line)
        m = HEREDOC.search(line)
        if m:
            end = m.group(2)
    return '\n'.join(out)


def strip_code(text, lang):
    """The text with comments, literal contents and preprocessor lines blanked; length and lines kept."""
    if lang == 'sh':
        text = _blank_heredocs(text)
    return TOKENS[lang].sub(_keep_quotes, text)


def _matching_open(h):
    """Index of the '(' that matches the last ')' of h, or -1."""
    depth = 0
    for i in range(len(h) - 1, -1, -1):
        if h[i] == ')':
            depth += 1
        elif h[i] == '(':
            depth -= 1
            if depth == 0:
                return i
    return -1


def _strip_template(h):
    """Drop leading 'template <...>' clauses."""
    while True:
        m = re.match(r'\s*template\s*<', h)
        if not m:
            return h
        depth = 0
        for j in range(m.end() - 1, len(h)):
            depth += (h[j] == '<') - (h[j] == '>')
            if depth == 0:
                break
        h = h[j + 1:]


def _cut_init_list(h):
    """Drop a constructor's ': a(x), b(y)' initializer list (a single colon at depth 0)."""
    depth = 0
    for i, ch in enumerate(h):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ':' and not depth and h[i - 1:i] != ':' and h[i + 1:i + 2] != ':':
            return h[:i] if h[:i].rstrip().endswith(')') else h
    return h


def _strip_trailers(h):
    """Drop trailing const, noexcept, override, '-> type' and similar."""
    while True:
        new = TRAILER.sub('', h)
        if new == h:
            return h
        h = new


def _classify_c(header):
    """Name of the C-like function whose header this is, or None (class, if, lambda, initialiser...)."""
    h = re.sub(r'^\s*(?:(?:public|private|protected)\s*:\s*)+', '', header)
    h = _strip_template(h).strip()
    first = re.match(r'(\w+)', h)
    if not h or (first and first.group(1) in NOT_FUNC_WORDS):
        return None
    h = _strip_trailers(_cut_init_list(h)).strip()
    if not h.endswith(')'):
        return None
    i = _matching_open(h)
    pre = h[:i].rstrip() if i >= 0 else ''
    if not pre or (pre.endswith(']') and not pre.endswith('operator[]')):
        return None
    m = NAME_RE.search(pre)
    if not m or m.group(1).split('::')[-1] in KEYWORDS:
        return None
    return m.group(1)


def _classify_ts(header):
    """Name of a TypeScript function (declaration, method, or block-bodied arrow), or None."""
    h = header.strip()
    m = TS_FUNC.match(h)
    if m:
        return m.group(1) or 'anonymous'
    if h.endswith('=>'):
        m = ARROW_NAME.search(h)
        return m.group(1) if m else 'arrow'
    h = re.sub(r'\)\s*:[^()]*$', ')', TS_MODIFIERS.sub('', h).strip())
    if not h.endswith(')'):
        return None
    i = _matching_open(h)
    m = re.search(r'([\w$]+)\s*(?:<[^()]*>)?$', h[:i].rstrip()) if i >= 0 else None
    if not m or m.group(1) in TS_NOT_FUNC:
        return None
    return m.group(1)


def classify(header, lang):
    """Function name for a brace block's header text, or None when the block is not a function."""
    if lang == 'sh':
        m = SH_FN.fullmatch(header.strip())
        return (m.group(1) or m.group(2)) if m else None
    if lang == 'ts':
        return _classify_ts(header)
    return _classify_c(header)


def _close(stack, funcs, line):
    """Pop the innermost brace block; a function block is recorded with its start and end lines."""
    if stack:
        name, start = stack.pop()
        if name:
            funcs.append((start, line, name))


def find_functions(code, lang):
    """Brace blocks whose header is a function definition, as (start_line, end_line, name)."""
    funcs, stack, buf = [], [], []
    head, depth, line = None, 0, 1
    for ch in code:
        if ch == '\n':
            line += 1
            if lang == 'sh' and not depth and not SH_FN.fullmatch(''.join(buf).strip()):
                buf, head = [], None
            elif head is not None:
                buf.append(' ')
        elif ch.isspace():
            if head is not None:
                buf.append(ch)
        elif ch == '{' and depth:          # inside a call: a lambda or braced value, part of the statement
            stack.append((None, 0))
            buf.append(ch)
        elif ch == '{':
            stack.append((classify(''.join(buf), lang), head or line))
            buf, head = [], None
        elif ch == '}':
            _close(stack, funcs, line)
            if depth:
                buf.append(ch)
            else:
                buf, head = [], None
        elif ch == ';' and not depth:
            buf, head = [], None
        else:
            if head is None:
                head = line
            depth = max(0, depth + (ch == '(') - (ch == ')'))
            buf.append(ch)
    return funcs


def scan_file(path, text, lang):
    """(line, lines, name) for every function in one file. Raises SyntaxError for bad Python."""
    if lang == 'py':
        tree = ast.parse(text, filename=path)
        return [(n.lineno, n.end_lineno - n.lineno + 1, n.name) for n in ast.walk(tree)
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
    return [(s, e - s + 1, name) for s, e, name in find_functions(strip_code(text, lang), lang)]


def line_count(text):
    """Number of lines (a last line without a newline counts)."""
    return text.count('\n') + (0 if not text or text.endswith('\n') else 1)


def change_size(root, base):
    """Added plus removed lines since base, from git diff --numstat (binary files skipped)."""
    total = 0
    for row in git(root, 'diff', '--numstat', base).splitlines():
        added, removed, _ = row.split('\t', 2)
        if added != '-':
            total += int(added) + int(removed)
    return total


def check_tree(root):
    """(violations, stats, unmeasured): violations are (size, text) pairs; stats count files and functions."""
    violations, unmeasured = [], []
    stats = {'files': 0, 'funcs': 0, 'funcs_over': 0, 'files_over': 0}
    for path in git(root, 'ls-files', '-z').split('\0'):
        name = os.path.basename(path)
        ext = os.path.splitext(name)[1]
        lang = LANG_BY_EXT.get(ext)
        if not path or skip_reason(path):
            continue
        if lang is None:
            if ext not in QUIET_EXT and name not in QUIET_NAMES and not name.startswith('.'):
                unmeasured.append(path)
            continue
        with open(os.path.join(root, path), encoding='utf-8', errors='replace') as f:
            text = f.read()
        stats['files'] += 1
        try:
            funcs = scan_file(path, text, lang)
        except SyntaxError as err:
            violations.append((10**9, f'{path}:{err.lineno or 1} PARSE ERROR {err.msg}'))
            continue
        n = line_count(text)
        if n > FILE_MAX:
            stats['files_over'] += 1
            violations.append((n, f'{path}:1 <file> {n} lines (limit {FILE_MAX})'))
        stats['funcs'] += len(funcs)
        for line, size, fname in funcs:
            if size > FUNC_MAX:
                stats['funcs_over'] += 1
                violations.append((size, f'{path}:{line} {fname} {size} lines (limit {FUNC_MAX})'))
    return violations, stats, unmeasured


def main():
    ap = argparse.ArgumentParser(description='Code size check (see the file header).')
    ap.add_argument('--base', help='ref for the change-size check, e.g. origin/main')
    ap.add_argument('--root', help='repository root (default: git top level of the cwd)')
    args = ap.parse_args()
    root = args.root or git('.', 'rev-parse', '--show-toplevel').strip()
    violations, stats, unmeasured = check_tree(root)
    change = None
    if args.base:
        change = change_size(root, args.base)
        if change > CHANGE_MAX:
            violations.append((change, f'<change since {args.base}>:0 <change> {change} lines '
                                       f'(limit {CHANGE_MAX})'))
    for _, text in sorted(violations, key=lambda v: -v[0]):
        print(text)
    summary = (f"summary: {stats['files']} files measured; {stats['files_over']} over {FILE_MAX} lines; "
               f"{stats['funcs_over']} of {stats['funcs']} functions over {FUNC_MAX} lines")
    if change is not None:
        summary += f'; change since {args.base}: {change} lines (limit {CHANGE_MAX})'
    print(summary)
    if unmeasured:
        print('not measured (language not in the list): ' + ' '.join(unmeasured))
    print('result: FAIL' if violations else 'result: PASS')
    return 1 if violations else 0


if __name__ == '__main__':
    sys.exit(main())
