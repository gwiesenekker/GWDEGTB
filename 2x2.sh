#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
log_dir=${EGTB_LOG_DIR:-"$script_dir/logs/2x2"}
threads=${EGTB_THREADS:-1}
sliced=${EGTB_SLICED:-}
[ "$sliced" = "--sliced" ] || sliced=

cd "$script_dir"
mkdir -p "$log_dir"

generate()
{
    white_kings=$1
    white_men=$2
    black_kings=$3
    black_men=$4
    name="${white_kings}wX-${white_men}wO-${black_kings}bX-${black_men}bO"
    revision=$(./generate_egtb --revision)
    revision=${revision##* }
    case "$revision" in
        ''|*[!0-9.]*) printf 'Invalid generator revision: %s\n' "$revision" >&2; exit 1 ;;
    esac
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    log="$log_dir/$name-rev$revision-$stamp-$$.log"

    printf 'Generating %s with %s thread(s) (log: %s)\n' \
        "$name" "$threads" "$log"
    if ./generate_egtb --restart $sliced -j "$threads" "$white_kings" "$white_men" \
            "$black_kings" "$black_men" >"$log" 2>&1; then
        sed -n '/^generated /,$p' "$log"
    else
        status=$?
        printf 'Generation failed for %s; final log lines follow:\n' \
            "$name" >&2
        tail -n 20 "$log" >&2
        exit "$status"
    fi
}

generate 2 0 2 0
generate 2 0 1 1
generate 2 0 0 2
generate 1 1 1 1
generate 1 1 0 2
generate 0 2 0 2

printf 'Completed all 2x2 EGTBs. Logs: %s\n' "$log_dir"
