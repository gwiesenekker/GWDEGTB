#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
log_dir=${EGTB_LOG_DIR:-"$script_dir/logs/5x2"}
threads=${EGTB_THREADS:-1}

cd "$script_dir"
mkdir -p "$log_dir"

generate()
{
    white_kings=$1
    white_men=$2
    black_kings=$3
    black_men=$4
    name="${white_kings}wX-${white_men}wO-${black_kings}bX-${black_men}bO"
    database="$script_dir/$name.dtm"
    log="$log_dir/$name.log"

    printf 'Generating %s (log: %s)\n' "$name" "$log"
    rm -f -- "$database"
    if ./generate_egtb -j "$threads" "$white_kings" "$white_men" \
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

generate 5 0 2 0
generate 5 0 1 1
generate 5 0 0 2
generate 4 1 2 0
generate 4 1 1 1
generate 4 1 0 2
generate 3 2 2 0
generate 3 2 1 1
generate 3 2 0 2
generate 2 3 2 0
generate 2 3 1 1
generate 2 3 0 2
generate 1 4 2 0
generate 1 4 1 1
generate 1 4 0 2
generate 0 5 2 0
generate 0 5 1 1
generate 0 5 0 2

printf 'Completed all 5x2 EGTBs. Logs: %s\n' "$log_dir"
