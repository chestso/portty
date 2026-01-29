#!/bin/bash

# Enable strict error handling
set -euo pipefail

# bloom-terminal Test Runner
# Run all tests or specific test categories

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VTERM_BIN="$PROJECT_ROOT/build/src/bloom-terminal"
VERBOSE="-v"

# Timeout configuration (in seconds)
DEFAULT_TIMEOUT=5
TIMEOUT=${VTERM_TEST_TIMEOUT:-$DEFAULT_TIMEOUT}

# Helper to detect shell scripts - only check for #!/bin/sh
is_shell_script() {
	local file="$1"
	if [ ! -f "$file" ]; then
		return 1
	fi
	# Check first line for #!/bin/sh ONLY (not #!/bin/bash)
	head -n 1 "$file" 2>/dev/null | grep -q "^#!/bin/sh"
	return $?
}

# Change to the script's directory to ensure relative paths work
cd "$SCRIPT_DIR"

if [ ! -f "$VTERM_BIN" ]; then
	echo "Error: bloom-terminal not found at $VTERM_BIN"
	echo "Checked absolute path: $VTERM_BIN"
	echo "Please build the project first:"
	echo "  From project root: ./build.sh -y"
	echo "  From examples dir: ../build.sh -y"
	exit 1
fi

run_test() {
	local test_file=$1
	local test_name=$2

	echo "========================================"
	echo "Running test: $test_name"
	echo "File: $test_file"
	echo "Timeout: ${TIMEOUT}s"
	echo "========================================"

	if [ ! -f "$test_file" ]; then
		echo "Error: Test file not found: $test_file" >&2
		return 1
	fi

	# Check if it's a shell script (starts with #!/bin/sh or #!/bin/bash)
	if ! head -n 1 "$test_file" | grep -q "^#!/bin/"; then
		echo "Warning: $test_file doesn't appear to be a shell script" >&2
	fi

	# Make sure it's executable
	if [ ! -x "$test_file" ]; then
		echo "Making $test_file executable..." >&2
		chmod +x "$test_file"
	fi

	# Check if timeout command is available
	if ! command -v timeout &>/dev/null; then
		echo "Error: 'timeout' command not found. Please install GNU coreutils." >&2
		return 1
	fi

	# Run the test script and pipe its output to bloom-terminal with timeout
	echo "DEBUG: Running pipeline: timeout $TIMEOUT \"$test_file\" | timeout $TIMEOUT "$VTERM_BIN" $VERBOSE -e -" >&2
	if timeout $TIMEOUT "$test_file" | timeout $TIMEOUT "$VTERM_BIN" $VERBOSE -e -; then
		# Success
		printf "\r\nTest completed: $test_name\r\n"
		printf "\r\n"
		return 0
	else
		exit_status=$?
		# Check exit status
		if [ $exit_status -eq 124 ]; then
			# Timeout
			echo "Error: Test '$test_name' timed out after ${TIMEOUT} seconds" >&2
			return 1
		else
			# Other failure
			echo "Error: Test '$test_name' failed with exit code $exit_status" >&2
			return 1
		fi
	fi
}

run_category() {
	local category=$1
	local category_dir="./$category"
	local found_files=0

	if [ ! -d "$category_dir" ]; then
		echo "Error: Category directory not found: $category_dir" >&2
		return 1
	fi

	echo "========================================"
	echo "Running all tests in category: $category"
	echo "========================================"

	# Look for all files (not just .txt) in the category directory
	for test_file in "$category_dir"/*; do
		# Skip if it's a directory
		if [ -d "$test_file" ]; then
			continue
		fi

		# Check if it's a shell script
		if ! is_shell_script "$test_file"; then
			continue
		fi

		# Check if it's a regular file
		if [ -f "$test_file" ]; then
			test_name=$(basename "$test_file")
			run_test "$test_file" "$test_name"
			sleep 1
			found_files=1
		fi
	done

	if [ $found_files -eq 0 ]; then
		echo "Warning: No test files found in $category_dir" >&2
	fi
}

list_tests() {
	echo "Available tests:"
	echo "================="
	echo

	# Find all directories (categories) in the current directory
	local has_categories=0
	local categories=()

	# Find directories, excluding '.' and special ones
	for dir in ./*/; do
		if [ -d "$dir" ]; then
			dir_name=$(basename "$dir")
			# Check if this directory contains any shell scripts
			local has_scripts=0
			for file in "$dir"*; do
				if [ -f "$file" ] && is_shell_script "$file"; then
					has_scripts=1
					break
				fi
			done
			if [ $has_scripts -eq 1 ]; then
				categories+=("$dir_name")
			fi
		fi
	done

	# Process each found category
	for category in "${categories[@]}"; do
		echo "Category: $category"
		# List all shell scripts in this category
		for file in "./$category"/*; do
			if [ -f "$file" ] && is_shell_script "$file"; then
				test_name=$(basename "$file")
				echo "  $test_name"
			fi
		done | sort
		echo
		has_categories=1
	done

	# Handle individual tests (files in current directory)
	local has_individual=0
	echo "Individual tests:"
	for file in ./*; do
		if [ -f "$file" ] && [ "$(basename "$file")" != "run-examples.sh" ] && is_shell_script "$file"; then
			test_name=$(basename "$file")
			echo "  $test_name"
			has_individual=1
		fi
	done | sort

	if [ $has_individual -eq 0 ]; then
		echo "  (none)"
	fi
	echo

	echo "Usage examples:"
	echo "  ./run-examples.sh basic/hello          # Run specific test"
	echo "  ./run-examples.sh basic                # Run all tests in category"
	echo "  ./run-examples.sh all                  # Run all-in-one test"
	echo "  ./run-examples.sh                      # Run all tests (default)"
	echo
	echo "Timeout: Default is ${DEFAULT_TIMEOUT} seconds"
	echo "         Override with VTERM_TEST_TIMEOUT environment variable"
	echo "         Example: VTERM_TEST_TIMEOUT=10 ./run-examples.sh basic/hello"
}

show_help() {
	echo "Usage: ./run-examples.sh [OPTIONS] [TEST_PATH]"
	echo
	echo "Run bloom-terminal tests"
	echo
	echo "Options:"
	echo "  -h, --help     Show this help message"
	echo "  -l, --list     List all available tests"
	echo "  -t SECONDS     Set timeout in seconds (default: ${DEFAULT_TIMEOUT})"
	echo "                 Can also use VTERM_TEST_TIMEOUT environment variable"
	echo
	echo "Arguments:"
	echo "  TEST_PATH      Path to test file or category directory"
	echo "                 Examples:"
	echo "                   basic/hello      # Specific test file"
	echo "                   basic            # All tests in 'basic' category"
	echo "                   all              # Run all-in-one test"
	echo "                 If not specified, runs all tests"
	echo
	echo "Examples:"
	echo "  ./run-examples.sh --list                     # List all tests"
	echo "  ./run-examples.sh basic/hello                # Run specific test"
	echo "  ./run-examples.sh basic                      # Run all tests in 'basic' category"
	echo "  ./run-examples.sh advanced/cursor_control    # Run specific advanced test"
	echo "  ./run-examples.sh                            # Run all tests (default)"
	echo
	echo "Environment variables:"
	echo "  VTERM_TEST_TIMEOUT    Timeout in seconds for each test"
}

# Parse command line arguments
SHOW_LIST=0
SHOW_HELP=0
TEST_PATH=""

# Manual argument parsing to handle mixed options and paths
while [ $# -gt 0 ]; do
	case "$1" in
	-h | --help)
		SHOW_HELP=1
		shift
		;;
	-l | --list)
		SHOW_LIST=1
		shift
		;;
	-t)
		if [ $# -gt 1 ]; then
			TIMEOUT="$2"
			shift 2
		else
			echo "Error: -t requires a timeout value" >&2
			exit 1
		fi
		;;
	--timeout=*)
		TIMEOUT="${1#*=}"
		shift
		;;
	-*)
		echo "Error: Unknown option: $1" >&2
		show_help
		exit 1
		;;
	*)
		# First non-option argument is the test path
		if [ -z "$TEST_PATH" ]; then
			TEST_PATH="$1"
		else
			echo "Error: Multiple test paths specified: '$TEST_PATH' and '$1'" >&2
			echo "Only one test path can be specified at a time" >&2
			exit 1
		fi
		shift
		;;
	esac
done

# Handle help and list options
if [ $SHOW_HELP -eq 1 ]; then
	show_help
	exit 0
fi

if [ $SHOW_LIST -eq 1 ]; then
	list_tests
	exit 0
fi

# Main test execution logic
if [ -n "$TEST_PATH" ]; then
	# Specific test path provided
	case "$TEST_PATH" in
	"all")
		# Run all-in-one test
		if [ -f "./all_in_one" ]; then
			run_test "./all_in_one" "All-in-One Test"
		else
			echo "Error: all_in_one test not found" >&2
			exit 1
		fi
		;;
	*)
		# Check if it's a file path
		if [ -f "$TEST_PATH" ]; then
			# Run specific test file
			test_name=$(basename "$TEST_PATH")
			run_test "$TEST_PATH" "$test_name"
		elif [ -d "$TEST_PATH" ]; then
			# Run directory as category
			category_name=$(basename "$TEST_PATH")
			run_category "$category_name"
		else
			# Check if it's a relative path within a category
			if [[ "$TEST_PATH" == */* ]]; then
				# Extract category and test name
				category="${TEST_PATH%%/*}"
				test_name="${TEST_PATH#*/}"
				# Check if the category exists and the test file exists
				if [ -d "./$category" ] && [ -f "./$category/$test_name" ]; then
					run_test "./$category/$test_name" "$test_name"
				else
					echo "Error: Test not found: $TEST_PATH" >&2
					echo "Use '$0 --list' to see available tests" >&2
					exit 1
				fi
			else
				echo "Error: Test not found: $TEST_PATH" >&2
				echo "Use '$0 --list' to see available tests" >&2
				exit 1
			fi
		fi
		;;
	esac
else
	# No arguments provided: run all tests by default
	echo "No test specified, running all tests..."
	echo

	# Find and run all categories
	for dir in ./*/; do
		if [ -d "$dir" ]; then
			category_name=$(basename "$dir")
			# Check if this category has any shell scripts
			has_scripts=0
			for file in "$dir"*; do
				if [ -f "$file" ] && is_shell_script "$file"; then
					has_scripts=1
					break
				fi
			done
			if [ $has_scripts -eq 1 ]; then
				run_category "$category_name"
			fi
		fi
	done

	# Run all-in-one test if it exists
	if [ -f "./all_in_one" ]; then
		run_test "./all_in_one" "All-in-One Test"
	fi
fi

echo "========================================"
echo "All tests completed!"
echo "========================================"
