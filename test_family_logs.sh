#!/bin/sh
set -eu
root=$(pwd)
work=$(mktemp -d /tmp/gwdegtb-family-logs-XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cat > "$work/generate_egtb" <<'STUB'
#!/bin/sh
if [ "$1" = --revision ]; then
    printf 'GWDEGTB revision 3.309\n'
else
    printf 'generated mock database\n'
fi
STUB
chmod +x "$work/generate_egtb"
for family in 1x1 2x1 2x2 3x1 3x2 4x1 3x3 4x2 5x1 4x3 5x2 6x1 4x4; do
    cp "$root/$family.sh" "$work/$family.sh"
    sh -n "$work/$family.sh"
    EGTB_LOG_DIR="$work/logs/$family" sh "$work/$family.sh" > /dev/null
done
EGTB_LOG_DIR="$work/logs/1x1" sh "$work/1x1.sh" > /dev/null
count=$(find "$work/logs/1x1" -type f -name '*-rev3.309-*.log' | wc -l)
[ "$count" -eq 6 ]
printf 'Family scripts: revision-specific logs and repeat-run preservation passed\n'
