#!/bin/sh
# Summarise a PORTTY_PACE_TRACE file as a frame-hold histogram.
#
#   scripts/pace-report.sh TRACE [VBLANK_NS]
#
# VBLANK_NS is the display period in ns (default 16666667, 60 Hz).
#
# A hold is how many display refreshes one animation frame stayed on screen.
# A phase-locked trace (locked = 1 on most lines) presents once per refresh,
# so a hold is the number of consecutive presents that showed the same frame.
# An unlocked trace commits at arbitrary times: the display is reconstructed
# as the last frame committed before each refresh, for four refresh phases.
#
# Only the first Lottie of each line (the first frame column) is considered.

if [ $# -lt 1 ] || [ ! -r "$1" ]; then
	echo "usage: $0 TRACE [VBLANK_NS]" >&2
	exit 2
fi
trace=$1
vblank=${2:-16666667}

awk -v P="$vblank" '
function addhold(k) {
	if (k > 7)
		k = 7
	h[k]++
}
function print_hist(tag,   k, line) {
	line = sprintf("%-14s holds(vblanks)", tag)
	for (k = 1; k <= 7; k++)
		if (h[k] > 0)
			line = line sprintf(" %d:%d", k, h[k])
	print line
	delete h
}
$1 == "P" && $8 >= 1 {
	n++
	t[n] = $3
	f[n] = $9
	if ($5 == 1)
		lockedn++
	if ($7 == 1)
		heldn++
	misses = $6
}
END {
	if (n < 4) {
		print "not enough presented frames" > "/dev/stderr"
		exit 1
	}
	printf "%d presents with a playing animation, %d locked, %d held, %d misses\n",
	    n, lockedn, heldn, misses
	if (lockedn * 2 > n) {
		# One present per refresh: a hold is a run of equal frames.
		run = 0
		cur = f[1]
		first = 1
		for (i = 1; i <= n; i++) {
			if (f[i] == cur) {
				run++
			} else {
				if (!first)
					addhold(run)
				first = 0
				cur = f[i]
				run = 1
			}
		}
		print_hist("locked")
		exit 0
	}
	for (ph = 0; ph < 4; ph++) {
		v = t[1] + int(ph * P / 4)
		k = 1
		shown = -1
		run = 0
		first = 1
		for (; v < t[n]; v += P) {
			while (k < n && t[k + 1] <= v)
				k++
			fr = (t[k] <= v) ? f[k] : -1
			if (fr == shown) {
				run++
			} else {
				if (shown >= 0 && !first)
					addhold(run)
				if (shown >= 0)
					first = 0
				shown = fr
				run = 1
			}
		}
		print_hist(sprintf("phase %d/4", ph))
	}
}
' "$trace"
