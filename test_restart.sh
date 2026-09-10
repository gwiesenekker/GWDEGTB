#!/bin/sh
set -eu
binary=${1:-"$(pwd)/generate_egtb"}
directory=$(mktemp -d /tmp/gwdegtb-restart-XXXXXX)
printf 'Restart test directory: %s\n' "$directory"
cd "$directory"
name=1wX-0wO-1bX-0bO.dtm
printf 'unfinished sentinel\n' > "$name.incomplete"
cp "$name.incomplete" expected
mkdir "$name.work"
printf 'checkpoint sentinel\n' > "$name.work/marker"

# Plain invocation cannot alter a retained file.
if "$binary" -j 2 1 0 1 0 > plain.log 2>&1; then exit 1; fi
cmp expected "$name.incomplete"
grep -q 'use --restart' plain.log

# Retry removes both outputs and preserves the workspace.
printf 'finished sentinel\n' > "$name"
"$binary" --restart -j 2 1 0 1 0 > retry.log 2>&1
test ! -e "$name.incomplete"
set -- "$name".incomplete.failed.*
test ! -e "$1"
grep -q 'checkpoint sentinel' "$name.work/marker"
test ! -e "$name.lock"
grep -q 'removed unfinished database' retry.log
grep -q 'removed previous finished database' retry.log
! grep -q '^  consistency repair\|^  final DTM scan\|^  generator total' retry.log
awk '
 /^wall-clock timings:/ { timing=1 }
 timing && /^  generation subtotal / { subtotal=NR }
 timing && /^  verify \+ statistics\/fallback / { verify=NR }
 timing && /^  storage metadata / { metadata=NR }
 timing && /^  durable publication / { publication=NR }
 timing && /^  total / { total=NR }
 END { exit !(subtotal>0 && subtotal<verify && verify<metadata && metadata<publication && publication<total) }
' retry.log

# Plain invocation still refuses an existing finished database.
cp "$name" finished-copy.dtm
if "$binary" -j 2 1 0 1 0 > finished.log 2>&1; then exit 1; fi
cmp finished-copy.dtm "$name"

# A second restart also succeeds without backups.
cp expected "$name.incomplete"
"$binary" --restart -j 2 1 0 1 0 > retry2.log 2>&1
set -- "$name".incomplete.failed.*
test ! -e "$1"
test ! -e "$name.incomplete"

# Symlinks are not interpreted as restart targets.
ln -s expected "$name.incomplete"
if "$binary" --restart -j 2 1 0 1 0 > symlink.log 2>&1; then exit 1; fi
test -L "$name.incomplete"
grep -q 'non-regular' symlink.log
test -f "$name"

# Real sliced resume retains completed checkpoints, not just a marker directory.
"$binary" -j 2 1 0 0 1 > dependency.log 2>&1
EGTB_KEEP_SLICES=1 "$binary" --sliced -j 2 0 1 0 1 > sliced.log 2>&1
cp 0wX-1wO-0bX-1bO.dtm sliced-original.dtm
printf 'interrupted merge\n' > 0wX-1wO-0bX-1bO.dtm.incomplete
EGTB_KEEP_SLICES=1 "$binary" --restart --sliced -j 2 0 1 0 1 > sliced-retry.log 2>&1
cmp sliced-original.dtm 0wX-1wO-0bX-1bO.dtm
test -d 0wX-1wO-0bX-1bO.dtm.work
grep -q '^  slice verification/fallback ' sliced.log
grep -q '^  full-index merge ' sliced.log
awk '
 /^[^ ]/ {
   mode=""
   if ($0 == "generator dependency caches:") mode="g"
   if ($0 == "final verification dependency caches:") mode="v"
   if ($0 ~ /^generator dependency caches by material/) mode="gd"
   if ($0 ~ /^final verification dependency caches by material/) mode="vd"
 }
 mode == "g" || mode == "v" {
   if ($1 == "Lookups") expected[mode,1]=$2
   if ($1 == "Misses") expected[mode,2]=$2
   if ($1 == "Decompressions") expected[mode,3]=$2
 }
 (mode == "gd" || mode == "vd") && $1 ~ /[.]dtm$/ {
   phase=substr(mode,1,1)
   sum[phase,1]+=$2; sum[phase,2]+=$3; sum[phase,3]+=$4
 }
 END {
   if (expected["g",1] == 0 || expected["v",1] == 0) exit 1
   for (i=1;i<=3;i++)
     if (sum["g",i] != expected["g",i] || sum["v",i] != expected["v",i]) exit 1
 }
' sliced.log
printf 'Restart tests: PASS (replacement, no backups/locks, checkpoints, symlink refusal)\n'
