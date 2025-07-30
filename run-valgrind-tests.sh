#!/usr/bin/env bash
#
# Script to run bitcoin tests under valgrind
# Usage: ./run-valgrind-tests.sh [ctest arguments]
#
# Examples:
#   ./run-valgrind-tests.sh                    # Run all tests
#   ./run-valgrind-tests.sh -R "rpc_*"        # Run only RPC tests
#   ./run-valgrind-tests.sh --timeout 600     # Set custom timeout

set -e

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
BUILD_BIN_DIR="${BUILD_BIN_DIR:-${BUILD_DIR}/bin}"
RESTORE_BINARIES="${RESTORE_BINARIES:-true}"

# Function to restore original binaries
restore_binaries() {
    echo -e "${YELLOW}Restoring original binaries...${NC}"
    for binary_orig in "$BUILD_BIN_DIR"/bitcoin*_orig "$BUILD_BIN_DIR"/test_*_orig; do
        [ -f "$binary_orig" ] || continue
        binary="${binary_orig%_orig}"
        if [ -f "$binary_orig" ]; then
            mv "$binary_orig" "$binary"
            echo "Restored: $(basename "$binary")"
        fi
    done
}

# Function to check if binaries are already wrapped
check_wrapped() {
    for binary in "$BUILD_BIN_DIR"/bitcoin* "$BUILD_BIN_DIR"/test_*; do
        [ -f "$binary" ] && [ -x "$binary" ] || continue
        [[ "$binary" == *"_orig" ]] && continue
        [[ "$binary" == *".sh" ]] && continue

        if [ -f "${binary}_orig" ]; then
            return 0  # Already wrapped
        fi
    done
    return 1  # Not wrapped
}

# Trap to ensure cleanup on exit
if [ "$RESTORE_BINARIES" = "true" ]; then
    trap restore_binaries EXIT
fi

echo -e "${GREEN}Bitcoin Valgrind Test Runner${NC}"
echo "================================"
echo "Build directory: $BUILD_DIR"
echo "Binary directory: $BUILD_BIN_DIR"
echo ""

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory $BUILD_DIR does not exist${NC}"
    echo "Please build the project first or set BUILD_DIR environment variable"
    exit 1
fi

# Check if binaries exist
if [ ! -f "$BUILD_BIN_DIR/bitcoind" ]; then
    echo -e "${RED}Error: bitcoind not found in $BUILD_BIN_DIR${NC}"
    echo "Please build the project first"
    exit 1
fi

# Check if already wrapped and offer to restore if needed
if check_wrapped; then
    echo -e "${YELLOW}Binaries appear to be already wrapped with valgrind${NC}"
    read -p "Do you want to restore and re-wrap? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        restore_binaries
    fi
fi

# Run the wrapper script
echo -e "${GREEN}Setting up valgrind wrappers...${NC}"
export BUILD_BIN_DIR
"$SCRIPT_DIR/wrap-valgrind-local.sh"

echo ""
echo -e "${GREEN}Running tests under valgrind...${NC}"
echo "This will be significantly slower than normal test execution."
echo ""

# Set environment variables that might be needed
export BOOST_TEST_RANDOM=${BOOST_TEST_RANDOM:-1}

# Run ctest with user-provided arguments or defaults
cd "$BUILD_DIR"
if [ $# -eq 0 ]; then
    # Default: run with 14 parallel jobs like the user requested
    echo "Running: ctest --test-dir . -j14"
    ctest --test-dir . -j14
else
    # Run with user-provided arguments
    echo "Running: ctest --test-dir . $@"
    ctest --test-dir . "$@"
fi

echo ""
echo -e "${GREEN}Tests completed!${NC}"

# Note: restore_binaries will be called automatically on exit if RESTORE_BINARIES=true
