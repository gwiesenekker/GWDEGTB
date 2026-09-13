#!/bin/sh
# Budget-constrained six-piece coordinator A/B; writes only a fresh /tmp directory.
set -eu
binary=$1
dependencies=$2
threads=${3:-16}
rounds=${4:-1}
directory=$(mktemp -d /tmp/gwdegtb-coordinator-XXXXXX)
printf 'Benchmark directory: %s\n' "$directory"
export EGTB_COMPRESSION_LEVEL=1 EGTB_COMPILATION_BUFFER_GIB=1
export EGTB_PAGE_SIZE=2048 EGTB_DEPENDENCY_RESIDENT_GIB=0
export EGTB_DEPENDENCY_RESIDENT_MAX_MIB=0 EGTB_RESIDENT_LIMIT_GIB=4
export EGTB_VERIFICATION_CACHE_GIB=1
export EGTB_DEPENDENCY_SHARED_CACHE_MIB=64 EGTB_DEPENDENCY_SHARED_CACHE_GIB=1
unset EGTB_SLICED EGTB_LEGACY_SCAN EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT
unset EGTB_DEPENDENCY_SHARED_CACHE_GROWTH
target=3wX-0wO-2bX-1bO
cd "$directory"
for file in "$dependencies"/*.dtm; do
    [ "${file##*/}" = "$target.dtm" ] || ln -s "$file" .
done
round=1
while [ "$round" -le "$rounds" ]; do
    modes='0 1'
    if [ $((round % 2)) -eq 0 ]; then modes='1 0'; fi
    for mode in $modes; do
        export EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE=$mode
        label="$round-rebalance-$mode"
        "$binary" --restart -j "$threads" 3 0 2 1 > "$label.log" 2>&1
        sed -n '/^WTM: wins=/,/^storage:/p' "$label.log" > "$label.stats"
        if [ -f reference.stats ]; then cmp reference.stats "$label.stats"; else cp "$label.stats" reference.stats; fi
        printf '%s: ' "$label"
        awk '/^  initialization |^  verify \+|^  total / {printf "%s ", $0} END {print ""}' "$label.log"
    done
    round=$((round+1))
done
printf 'All runs verified and statistics matched. Inspect coordinator counts before attributing a benefit.\n'
