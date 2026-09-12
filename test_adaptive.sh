#!/bin/sh
set -eu
binary=${1:-"$(pwd)/generate_egtb"}
verify=${2:-"$(pwd)/verify_dtm"}
compare=${3:-"$(pwd)/test_dependency_resident"}
directory=$(mktemp -d /tmp/gwdegtb-adaptive-XXXXXX)
printf 'Adaptive test directory: %s\n' "$directory"
cd "$directory"
export EGTB_DEPENDENCY_RESIDENT_GIB=0
export EGTB_DEPENDENCY_SHARED_CACHE_MIB=0
export EGTB_DEPENDENCY_SHARED_CACHE_GIB=0
for material in '1 0 1 0' '1 0 0 1' '0 1 0 1' \
                '2 0 1 0' '2 0 0 1' '1 1 1 0' '1 1 0 1' '0 2 1 0' '0 2 0 1' \
                '2 0 2 0' '2 0 1 1' '1 1 1 1'; do
    # These are fixed numeric argument lists, intentionally word-split.
    "$binary" -j 2 $material > "baseline-$material.log" 2>&1
done
mv 1wX-1wO-1bX-1bO.dtm baseline.dtm
export EGTB_DEPENDENCY_SHARED_CACHE_MIB=1
export EGTB_DEPENDENCY_SHARED_CACHE_GIB=1
"$binary" -j 2 1 1 1 1 > adaptive.log 2>&1
grep -q 'shared dependency cache:.*grew' adaptive.log
"$compare" baseline.dtm 1wX-1wO-1bX-1bO.dtm
"$verify" -d "$directory" -j 2 1 1 1 1 > verify.log 2>&1
grep -q 'DTM-values-checked=8956320' verify.log
printf 'Adaptive scan-round generation and verification tests passed\n'
