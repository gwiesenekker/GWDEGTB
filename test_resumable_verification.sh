#!/bin/sh
set -eu
generator=${1:-"$(pwd)/generate_egtb"}
verifier=${2:-"$(pwd)/verify_dtm"}
work=$(mktemp -d /tmp/egtb-resume-test-XXXXXX)
cd "$work"
export EGTB_PROGRESS_SECONDS=0 EGTB_RESIDENT_LIMIT_GIB=0
"$generator" -j 2 1 0 1 0 >generate.log 2>&1
if "$verifier" --checkpoint "$work/verify.state" --chunk-pages 1 \
        --max-chunks 1 -j 2 1 0 1 0 >first.log 2>&1; then
    echo 'expected an incomplete verification status' >&2; exit 1
else
    [ "$?" -eq 2 ]
fi
grep -q 'verified through index 1024/2450' first.log
"$verifier" --checkpoint "$work/verify.state" --chunk-pages 1 \
    -j 2 1 0 1 0 >resume.log 2>&1
grep -q 'resuming verification at index 1024/2450' resume.log
grep -q 'DTM-values-checked=4900 newly-checked=2852 checkpointed=2048' resume.log
EGTB_RESIDENT_LIMIT_GIB=1 "$verifier" --checkpoint "$work/resident.state" \
    --chunk-pages 2 -j 2 1 0 1 0 >resident.log 2>&1
grep -q 'DTM-values-checked=4900 newly-checked=4900' resident.log
grep -q 'mode=resident' resident.log
"$verifier" --checkpoint "$work/verify.state" --chunk-pages 1 \
    -j 2 1 0 1 0 >complete.log 2>&1
grep -q 'checkpoint covers all 4900 DTM values' complete.log
touch 1wX-0wO-1bX-0bO.dtm
if "$verifier" --checkpoint "$work/verify.state" --chunk-pages 1 \
        -j 2 1 0 1 0 >changed.log 2>&1; then
    echo 'changed DTM accepted' >&2; exit 1
fi
grep -q 'DTM inputs changed' changed.log
printf 'broken\n' >verify.state
if "$verifier" --checkpoint "$work/verify.state" --chunk-pages 1 \
        -j 2 1 0 1 0 >corrupt.log 2>&1; then
    echo 'corrupt checkpoint accepted' >&2; exit 1
fi
grep -q 'checkpoint is corrupt' corrupt.log
echo 'Resumable verification tests passed'
