#!/bin/sh
# Compare two built generators on identical, already generated dependencies.
# Run on an otherwise idle host; logs and results are retained under /tmp.
set -eu
old=$1
new=$2
dependencies=$3
rounds=${4:-3}
directory=$(mktemp -d /tmp/gwdegtb-policy-XXXXXX)
printf 'Benchmark directory: %s\n' "$directory"
export EGTB_COMPRESSION_LEVEL=1 EGTB_COMPILATION_BUFFER_GIB=1
export EGTB_PAGE_SIZE=2048
export EGTB_DEPENDENCY_RESIDENT_GIB=0 EGTB_DEPENDENCY_RESIDENT_MAX_MIB=0
export EGTB_RESIDENT_LIMIT_GIB=1 EGTB_VERIFICATION_CACHE_GIB=1
unset EGTB_SLICED EGTB_LEGACY_SCAN
target=1wX-2wO-2bX-0bO
cd "$directory"
for file in "$dependencies"/*.dtm; do
    [ "${file##*/}" = "$target.dtm" ] || ln -s "$file" .
done
run()
{
    label=$1 binary=$2 mode=$3
    case "$mode" in
        fixed64) export EGTB_DEPENDENCY_SHARED_CACHE_MIB=64 EGTB_DEPENDENCY_SHARED_CACHE_GIB=0 ;;
        adaptive64) export EGTB_DEPENDENCY_SHARED_CACHE_MIB=64 EGTB_DEPENDENCY_SHARED_CACHE_GIB=1 ;;
        adaptive1) export EGTB_DEPENDENCY_SHARED_CACHE_MIB=1 EGTB_DEPENDENCY_SHARED_CACHE_GIB=1 ;;
    esac
    "$binary" --restart -j 2 1 2 2 0 > "$label.log" 2>&1
    sed -n '/^WTM: wins=/,/^storage:/p' "$label.log" > "$label.stats"
    if [ -f reference.stats ]; then cmp reference.stats "$label.stats"; else cp "$label.stats" reference.stats; fi
    printf '%s: ' "$label"
    awk '/^  initialization |^  verify \+|^  total / {printf "%s ", $0} END {print ""}' "$label.log"
}
run warmup "$old" adaptive64
round=1
while [ "$round" -le "$rounds" ]; do
    for mode in fixed64 adaptive64 adaptive1; do
        if [ $((round % 2)) -eq 1 ]; then
            run "$round-$mode-old" "$old" "$mode"
            run "$round-$mode-new" "$new" "$mode"
        else
            run "$round-$mode-new" "$new" "$mode"
            run "$round-$mode-old" "$old" "$mode"
        fi
    done
    round=$((round+1))
done
printf 'All runs verified and all histograms/examples/storage statistics matched.\n'
