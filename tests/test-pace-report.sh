#!/bin/sh
# pace-report.sh turns a pace trace into a hold histogram. The traces below are
# built so the right answer is known.

srcdir=${srcdir:-.}
report=$srcdir/../scripts/pace-report.sh
tmp=${TMPDIR:-/tmp}/portty-pace-report.$$
trap 'rm -f "$tmp".*' EXIT

fail=0
check() {
	name=$1 want=$2 got=$3
	case $got in
	*"$want"*) ;;
	*)
		echo "FAIL: $name: wanted '$want' in:" >&2
		echo "$got" >&2
		fail=1
		;;
	esac
}

P=16666667

# Locked trace: one present per refresh, 30 fps content -> every frame twice.
{
	i=0
	while [ $i -lt 40 ]; do
		t=$((i * P))
		echo "P $t $t 0 1 0 0 1 $((i / 2))"
		i=$((i + 1))
	done
} >"$tmp.locked"
out=$("$report" "$tmp.locked" $P)
check "locked 30fps" "holds(vblanks) 2:18" "$out"

# Unlocked trace committing exactly on a 33.3 ms cadence mid-refresh:
# against a 16.67 ms refresh every frame is held for 2 refreshes.
{
	i=0
	while [ $i -lt 40 ]; do
		t=$((i * 2 * P + 4000000))
		echo "P $t $t 0 0 0 0 1 $i"
		i=$((i + 1))
	done
} >"$tmp.free"
out=$("$report" "$tmp.free" $P)
check "free 30fps phase 0" "phase 0/4" "$out"
case $out in
*" 1:"* | *" 3:"*)
	echo "FAIL: free 30fps has off-cadence holds:" >&2
	echo "$out" >&2
	fail=1
	;;
esac

# A frame held three refreshes shows up as a 3.
{
	i=0
	for fr in 0 1 1 1 2 3 4 5; do
		t=$((i * P))
		echo "P $t $t 0 1 0 0 1 $fr"
		i=$((i + 1))
	done
} >"$tmp.hold3"
out=$("$report" "$tmp.hold3" $P)
check "hold of 3" " 3:1" "$out"

# Too short a trace is an error, not a histogram.
echo "P 1 2 0 0 0 0 1 0" >"$tmp.short"
if "$report" "$tmp.short" $P >/dev/null 2>&1; then
	echo "FAIL: short trace should fail" >&2
	fail=1
fi

[ $fail -eq 0 ] && echo "PASS"
exit $fail
