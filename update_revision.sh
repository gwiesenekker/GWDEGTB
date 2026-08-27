#!/bin/sh

set -eu

state_file=.build-revision-state
source_file=revision.c
head_revision=$(git rev-parse --verify HEAD 2>/dev/null || printf '%s' nogit)
base_tag=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null ||
           printf '%s' v1.7)
base_revision=${base_tag#v}
previous_head=
build_number=0

if test -f "$state_file"; then
    read -r previous_head build_number < "$state_file" || true
fi
if test "$previous_head" = "$head_revision"; then
    build_number=$((build_number + 1))
else
    build_number=1
fi

revision=$(LC_ALL=C awk -v base="$base_revision" -v build="$build_number" \
    'BEGIN { printf "%.3f", base + build / 1000.0 }')

state_tmp=${state_file}.tmp
source_tmp=${source_file}.tmp
printf '%s %s\n' "$head_revision" "$build_number" > "$state_tmp"
{
    printf '%s\n' '#include "revision.h"'
    printf 'const char gwdegtb_revision[] = "%s";\n' "$revision"
} > "$source_tmp"
mv "$state_tmp" "$state_file"
mv "$source_tmp" "$source_file"
