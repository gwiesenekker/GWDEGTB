#!/bin/sh
# Real dependency databases, isolated outputs, alternating order; no source DTM writes.
# Usage: sh benchmark_slice_order.sh /absolute/generator /dependencies [rounds] [threads]
set -eu
binary=$1
dependencies=$2
rounds=${3:-3}
threads=${4:-16}
root=$(mktemp -d /tmp/gwdegtb-slice-order-XXXXXX)
printf 'Benchmark directory: %s\n' "$root"
export EGTB_COMPRESSION_LEVEL=1 EGTB_COMPILATION_BUFFER_GIB=1 EGTB_PAGE_SIZE=2048
export EGTB_DEPENDENCY_RESIDENT_GIB=0 EGTB_DEPENDENCY_RESIDENT_MAX_MIB=0
export EGTB_DEPENDENCY_SHARED_CACHE_MIB=1 EGTB_DEPENDENCY_SHARED_CACHE_GIB=0
export EGTB_RESIDENT_LIMIT_GIB=1 EGTB_VERIFICATION_CACHE_GIB=1
unset EGTB_KEEP_SLICES EGTB_LEGACY_SCAN
target=2wX-1wO-1bX-1bO
run() {
    label=$1 order=$2
    mkdir "$root/$label"
    for file in "$dependencies"/*.dtm; do
        [ "${file##*/}" = "$target.dtm" ] || ln -s "$file" "$root/$label/"
    done
    if [ "$order" = unsliced ]; then sliced=; else sliced=--sliced; fi
    (cd "$root/$label" && EGTB_SLICE_ORDER="$order" "$binary" $sliced \
        --verification full -j "$threads" 2 1 1 1 > "$root/$label.log" 2>&1)
    awk '/^WTM: /,/^storage:/ {if (/^(WTM|BTM): / || /^ *-?[0-9]+ +[0-9]+$/) print}' \
        "$root/$label.log" > "$root/$label.stats"
    if [ -f "$root/reference.stats" ]; then
        cmp "$root/reference.stats" "$root/$label.stats"
    else cp "$root/$label.stats" "$root/reference.stats"; fi
    printf '%s: ' "$label"
    awk '/^  initialization |^  backpropagation |^  frontier compilation |^  slice merge |^  total / {printf "%s; ", $0} END {print ""}' "$root/$label.log"
}
run warmup unsliced
round=1
while [ "$round" -le "$rounds" ]; do
    if [ $((round % 2)) -eq 1 ]; then orders='row column tile2 tile3 diagonal';
    else orders='diagonal tile3 tile2 column row'; fi
    for order in $orders; do run "$round-$order" "$order"; done
    round=$((round+1))
done
printf 'All runs fully verified; histograms match unsliced reference. Results: %s\n' "$root"
