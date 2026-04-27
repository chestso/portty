#!/bin/bash
# profile.sh - build with gprof instrumentation, run a benchmark, generate
# a profile report.
#
# Output: profile-report.txt (full gprof output), prints top-20 functions.

set -eu

cd "$(dirname "$0")/.."

BUILD_DIR="build-profile"

[ -f configure ] || ./autogen.sh

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

../configure --prefix="$HOME/.local" CFLAGS='-O2 -g -pg' LDFLAGS='-pg'
make -j"$(nproc)"
cd ..

BENCH_SCRIPT="$(mktemp --suffix=.sh)"
trap 'rm -f "$BENCH_SCRIPT"' EXIT

cat >"$BENCH_SCRIPT" <<'SCRIPT'
#!/bin/bash
# Bench script: exercises colors, scrollback, emoji, box drawing.
echo -e "\e[31mRed\e[32mGreen\e[34mBlue\e[0m"
echo -e "\e[1;33mBold Yellow\e[0m and \e[4;36mUnderline Cyan\e[0m"
for i in $(seq 1 50); do echo "Line $i: The quick brown fox jumps over the lazy dog"; done
for i in $(seq 0 5 255); do printf "\e[38;2;${i};$((255 - i));128m#\e[0m"; done
echo
echo "Emoji: 😀🎉🚀💻🔥✨🌍🎨📚🔧"
echo "┌────────────────────┐"
echo "│ Box drawing test   │"
echo "└────────────────────┘"
for i in $(seq 1 200); do echo "Scroll $i: Lorem ipsum dolor sit amet"; done
SCRIPT
chmod +x "$BENCH_SCRIPT"

echo "Running benchmark..."
rm -f gmon.out
"./${BUILD_DIR}/src/bloom-terminal" -- bash "$BENCH_SCRIPT"

if [ ! -f gmon.out ]; then
	echo "ERROR: gmon.out not generated (profiling data missing)" >&2
	exit 1
fi

REPORT="profile-report.txt"
gprof "./${BUILD_DIR}/src/bloom-terminal" gmon.out >"$REPORT"

echo
echo "=== Top 20 Functions by Cumulative Time ==="
head -30 "$REPORT" | tail -20
echo
echo "Full report: $REPORT"
