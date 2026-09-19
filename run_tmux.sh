#!/bin/bash
# Run a complete generation chain in a persistent remote terminal.
set -euo pipefail

session=egtb
socket=()
while (($#)); do
    case $1 in
        -s|-L)
            if (($# < 2)); then echo "Missing argument for $1" >&2; exit 2; fi
            if [[ $1 == -s ]]; then session=$2; else socket=(-L "$2"); fi
            shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
if (($# == 0)); then
    echo 'Usage: ./run_tmux.sh [-s SESSION] [-L SOCKET] [--] COMMAND [ARGUMENTS...]' >&2
    echo 'Example: ./run_tmux.sh numactl --interleave=all ./all.sh' >&2
    exit 2
fi
if [[ ! $session =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo 'Session name must contain only letters, digits, underscores or hyphens.' >&2
    exit 2
fi
if [[ -n ${TMUX:-} ]]; then
    echo 'Already inside tmux; running directly in the current pane.'
    exec "$@"
fi
command -v tmux >/dev/null || { echo 'tmux is not installed on this host.' >&2; exit 1; }
if tmux "${socket[@]}" has-session -t "=$session" 2>/dev/null; then
    echo "Session '$session' already exists; no new job started." >&2
    printf 'Attach with: tmux ' >&2
    printf '%q ' "${socket[@]}" attach-session -t "=$session" >&2
    printf '\n' >&2
    exit 1
fi

# An existing tmux server can have an old environment. Explicitly pass all
# exported EGTB_* settings and PATH from THIS invocation, without copying secrets.
# Clear stale EGTB_* settings inherited from that server first.
settings=("PATH=$PATH")
while IFS= read -r name; do
    [[ $name == EGTB_* ]] && settings+=("$name=${!name}")
done < <(compgen -e)
printf -v launch '%q ' env "${settings[@]}" "$@"
runner='for name in ${!EGTB_@}; do unset "$name"; done; '
runner+="$launch"
runner+='; status=$?; printf "\nGeneration command exited with status %s. Session retained.\n" "$status"; exec bash'
printf -v pane_command '%q ' bash -c "$runner"

# new-session itself atomically rejects duplicate names (including races).
tmux "${socket[@]}" new-session -d -s "$session" -c "$PWD" "$pane_command"
printf "Started session '%s'. Detach with Ctrl+B, then D.\n" "$session"
if [[ -t 0 && -t 1 ]]; then
    exec tmux "${socket[@]}" attach-session -t "=$session"
fi
printf 'Attach with: tmux '
printf '%q ' "${socket[@]}" attach-session -t "=$session"
printf '\n'
