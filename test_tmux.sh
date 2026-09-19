#!/bin/bash
set -euo pipefail
command -v tmux >/dev/null || { echo 'tmux tests skipped: tmux not installed'; exit 0; }
root=$PWD
work=$(mktemp -d /tmp/gwdegtb-tmux-XXXXXX)
socket="gwdegtb-test-${work##*/}"
cleanup() {
    tmux -L "$socket" kill-server 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
unset TMUX
# Start a server with stale settings; the launcher must not use those values.
EGTB_THREADS=99 EGTB_TEST_STALE=1 tmux -L "$socket" new-session -d -s seed
cd "$work"
EGTB_THREADS=7 EGTB_TEST_TEXT='spaces ; literal $value' bash "$root/run_tmux.sh" -L "$socket" -- \
    bash -c 'printf "%s\n" "$EGTB_THREADS" "$EGTB_TEST_TEXT" "${EGTB_TEST_STALE-unset}" "$PWD" "$1" > result; exit 7' \
    bash 'argument with spaces ; $(false)' > launch.log
for ((i=0;i<100;i++)); do
    [[ -f result ]] && break
    sleep .05
done
printf '%s\n' 7 'spaces ; literal $value' unset "$work" 'argument with spaces ; $(false)' > expected
cmp result expected
for ((i=0;i<100;i++)); do
    tmux -L "$socket" capture-pane -p -t '=egtb:' > pane.log
    grep -q 'exited with status 7' pane.log && break
    sleep .05
done
grep -q 'exited with status 7' pane.log
tmux -L "$socket" has-session -t '=egtb'
if bash "$root/run_tmux.sh" -L "$socket" touch duplicate > duplicate.log 2>&1; then
    echo 'Duplicate session unexpectedly accepted' >&2; exit 1
fi
[[ ! -e duplicate ]]
TMUX=test bash "$root/run_tmux.sh" bash -c 'printf nested > nested-result'
[[ $(<nested-result) == nested ]]
if bash "$root/run_tmux.sh" -s bad.name true >/dev/null 2>&1; then exit 1; fi
if bash "$root/run_tmux.sh" >/dev/null 2>&1; then exit 1; fi
echo 'tmux launcher: environment, quoting, working directory, retained failure, duplicate and nested tests passed'
