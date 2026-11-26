#!/bin/sh
# File and text drops paste as shell-quoted paths. End-to-end through the
# debug script `drop` / `drop-text` commands: delivery, multi-file joining,
# bracketed-paste mode respected, and the quoting boundary — a hostile
# filename must stay one literal shell word.

case "$(uname -s 2>/dev/null)" in
MSYS* | MINGW* | CYGWIN*) exit 77 ;;
esac

srcdir=${srcdir:-.}
bin=${PORTTY_BIN:-}
if [ -z "$bin" ]; then
	if [ -x "$srcdir/../src/portty" ]; then
		bin=$srcdir/../src/portty
	elif [ -x src/portty ]; then
		bin=src/portty
	elif [ -x ../src/portty ]; then
		bin=../src/portty
	else
		echo "FAIL: portty binary not found (set PORTTY_BIN)" >&2
		exit 77
	fi
fi
[ -x "$bin" ] || exit 77

# Scratch lives directly under /tmp, not TMPDIR: wait-for matches within
# one grid row, and macOS TMPDIR alone is ~65 characters — the joined
# payload would wrap at 80 columns and never match.
tmp=/tmp/portty-drop.$$
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp" || exit 77

# The binary must start in dummy video mode at all.
printf 'quit\n' >"$tmp/quit.sh"
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/quit.sh" >/dev/null 2>&1 || exit 77

fail=0
touch "$tmp/a b.txt" "$tmp/c.txt"

# Delivery at an interactive shell prompt: a single path, a multi-file
# batch with spaces, and dropped text. Ctrl+C (raw 03) clears the line
# between drops so pastes never concatenate. A short prompt keeps the
# joined payload inside one grid row.
cat >"$tmp/paste.sh" <<EOF
sendln export PS1='> '
wait 0.3
drop $tmp/c.txt
wait-for "'$tmp/c.txt'" 10
assert-not-contains 200~
raw 03
wait 0.3
drop "$tmp/a b.txt" $tmp/c.txt
wait-for "'$tmp/a b.txt' '$tmp/c.txt'" 10
raw 03
wait 0.3
drop-text hello dropped text
wait-for "hello dropped text" 10
quit
EOF
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/paste.sh" >"$tmp/paste.log" 2>&1 || {
	echo "FAIL: paste delivery run failed" >&2
	cat "$tmp/paste.log" >&2
	fail=1
}

# The quoting boundary: a filename that is shell syntax must arrive as
# one literal word. Executing the pasted line must fail with "no such
# file", never run the injected touch. The shell cd's into $tmp so the
# injected relative-path touch would land there if quoting ever broke.
evil="$tmp/x;touch PWNED"
touch "$evil" || exit 77
cat >"$tmp/evil.sh" <<EOF
sendln cd $tmp
sendln export PS1='> '
wait 0.3
drop "$evil"
wait-for "'$evil'" 10
raw 0d
wait 0.5
quit
EOF
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/evil.sh" >"$tmp/evil.log" 2>&1 || {
	echo "FAIL: hostile-name run failed" >&2
	cat "$tmp/evil.log" >&2
	fail=1
}
if [ -e "$tmp/PWNED" ]; then
	echo "FAIL: hostile filename executed as shell syntax" >&2
	fail=1
fi

# A child that never enables bracketed paste (cat) must receive the
# path without bracket bytes — the mode gates the wrapping.
cat >"$tmp/cat.sh" <<EOF
drop $tmp/c.txt
wait-for "'$tmp/c.txt'" 10
raw 0d
wait 0.3
assert-not-contains 200~
quit
EOF
SDL_VIDEODRIVER=dummy "$bin" -S "$tmp/cat.sh" -- cat >"$tmp/cat.log" 2>&1 || {
	echo "FAIL: unbracketed child run failed" >&2
	cat "$tmp/cat.log" >&2
	fail=1
}

[ $fail -eq 0 ] && echo "PASS"
exit $fail
