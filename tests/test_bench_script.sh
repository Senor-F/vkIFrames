#!/usr/bin/env bash
# shellcheck disable=SC2016,SC2034  # the test commands are in single quotes on purpose (eval) and use the variables there
# scripts/vkiframes-bench.sh in a throw-away HOME: it writes bench.req (temp file + rename), --cancel writes 0, and it refuses a symbolic link at the
# config folder or at bench.req before it creates or changes anything. No GPU, no game. usage: test_bench_script.sh <repo root>
set -u
REPO=${1:?usage: test_bench_script.sh <repo root>}
T=$(mktemp -d /tmp/vkibench.XXXXXX); trap 'rm -rf "$T"' EXIT
fails=0
ok() { if eval "$2"; then :; else echo "FAIL: $1 [$2]"; fails=$((fails+1)); fi; }
S="$REPO/scripts/vkiframes-bench.sh"
export HOME="$T/home dir"; mkdir -p "$HOME"; unset XDG_CONFIG_HOME
C="$HOME/.config/vkiframes"

ok "--no-wait exits 0" 'bash "$S" --no-wait >/dev/null'
ok "bench.req is one number, mode 600, folder 700" '[ "$(wc -l < "$C/bench.req")" = 1 ] && [ "$(stat -c %a "$C/bench.req")" = 600 ] && [ "$(stat -c %a "$C")" = 700 ] && grep -Eq "^[1-9][0-9]*$" "$C/bench.req"'
ok "no temporary file left" '[ "$(ls -A "$C")" = "bench.req" ]'
ok "--cancel writes 0" 'bash "$S" --cancel >/dev/null && [ "$(cat "$C/bench.req")" = 0 ]'
ok "an unknown argument is refused (exit 2)" 'bash "$S" --bogus >/dev/null 2>&1; [ $? -eq 2 ]'

# a symbolic link at the folder: refused, nothing written or changed behind it
H2="$T/home2"; mkdir -p "$H2/.config" "$T/elsewhere"; chmod 755 "$T/elsewhere"; ln -s "$T/elsewhere" "$H2/.config/vkiframes"
ok "a symlinked config folder is refused (exit 1, message on stderr)" 'HOME="$H2" bash "$S" --no-wait >/dev/null 2>"$T/err"; [ $? -eq 1 ] && grep -q "symbolic link" "$T/err"'
ok "... nothing was written and the target keeps its mode" '[ -z "$(ls -A "$T/elsewhere")" ] && [ "$(stat -c %a "$T/elsewhere")" = 755 ]'

# a symbolic link at bench.req: refused, the target is not replaced
H3="$T/home3"; mkdir -p "$H3/.config/vkiframes"; printf 'keep\n' > "$T/target"; ln -s "$T/target" "$H3/.config/vkiframes/bench.req"
ok "a symlinked bench.req is refused (exit 1)" 'HOME="$H3" bash "$S" --cancel >/dev/null 2>&1; [ $? -eq 1 ]'
ok "... the link and its target are unchanged" '[ -L "$H3/.config/vkiframes/bench.req" ] && [ "$(cat "$T/target")" = keep ]'

# XDG_CONFIG_HOME is honoured
ok "XDG_CONFIG_HOME is used" 'XDG_CONFIG_HOME="$T/xdg" bash "$S" --no-wait >/dev/null && [ -f "$T/xdg/vkiframes/bench.req" ]'
if [ "$fails" -eq 0 ]; then echo "bench script: ok"; else echo "bench script: $fails FAILED"; exit 1; fi
