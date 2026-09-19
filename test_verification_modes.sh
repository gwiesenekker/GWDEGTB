#!/bin/sh
set -eu
generator=${1:-"$(pwd)/generate_egtb"}
verifier=${2:-"$(pwd)/verify_dtm"}
work=$(mktemp -d /tmp/egtb-verification-modes-XXXXXX)
echo "Verification-mode test workspace: $work"
cd "$work"
export EGTB_PROGRESS_SECONDS=0
for mode in full slices none; do
    mkdir "$mode"
    cd "$mode"
    for material in '1 0 1 0' '1 0 0 1'; do
        # Intentional splitting of the four material counts.
        "$generator" --verification "$mode" -j 2 $material >"$material.log" 2>&1
        if [ "$mode" = slices ]; then
            grep -q 'positions-checked=' "$material.log"
            if grep -q 'NOT VERIFIED' "$material.log"; then exit 1; fi
        fi
    done
    EGTB_KEEP_SLICES=1 "$generator" --sliced --verification "$mode" -j 2 0 1 0 1 >men.log 2>&1
    "$verifier" -j 2 0 1 0 1 >verify.log 2>&1
    sed -n '/^WTM:/,/^storage:/p' men.log >histogram
    if [ "$mode" != full ]; then cmp histogram ../full/histogram; fi
    if [ "$mode" = slices ]; then
        grep -q 'SKIPPED' men.log
        grep -q 'slice verification: ' men.log
    fi
    if [ "$mode" = none ]; then
        grep -q 'NOT VERIFIED' men.log
        # Retain checkpoints while removing only the disposable test output.
        mv 0wX-1wO-0bX-1bO.dtm saved.dtm
        if "$generator" --sliced --verification full -j 2 0 1 0 1 >rejected.log 2>&1; then
            echo 'ERROR: unchecked checkpoints accepted' >&2; exit 1
        fi
        grep -q 'is NOT VERIFIED' rejected.log
        EGTB_KEEP_SLICES=1 "$generator" --sliced --verification none -j 2 0 1 0 1 >resume.log 2>&1
        grep -q 'already complete' resume.log
    fi
    cd ..
done
if EGTB_VERIFICATION=invalid "$generator" 1 0 1 0 >invalid.log 2>&1; then exit 1; fi
mkdir override
cd override
EGTB_VERIFICATION=none "$generator" --verification full 1 0 1 0 >override.log 2>&1
grep -q 'verification mode: full' override.log
if grep -q 'NOT VERIFIED' override.log; then exit 1; fi
echo 'Verification mode tests passed'
