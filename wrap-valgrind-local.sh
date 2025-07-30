#!/usr/bin/env bash
#
# Script to wrap bitcoin executables with valgrind for local testing
# Based on ci/test/wrap-valgrind.sh

set -ex

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Default to build/bin if BUILD_BIN_DIR not set
BUILD_BIN_DIR="${BUILD_BIN_DIR:-${SCRIPT_DIR}/build/bin}"

# Check if build directory exists
if [ ! -d "$BUILD_BIN_DIR" ]; then
    echo "Error: Build directory $BUILD_BIN_DIR does not exist"
    echo "Please build the project first or set BUILD_BIN_DIR environment variable"
    exit 1
fi

echo "Wrapping executables in $BUILD_BIN_DIR with valgrind..."

# Find all bitcoin executables and test executables in the build directory
for binary in "$BUILD_BIN_DIR"/bitcoin* "$BUILD_BIN_DIR"/test_*; do
    # Skip if not a file or not executable
    [ -f "$binary" ] && [ -x "$binary" ] || continue

    # Skip if already wrapped (ends with _orig)
    [[ "$binary" == *"_orig" ]] && continue

    # Skip if it's our wrapper script
    [[ "$binary" == *".sh" ]] && continue

    echo "Wrapping $binary ..."

    # Move original binary
    if [ ! -f "${binary}_orig" ]; then
        mv "$binary" "${binary}_orig"
    fi

    # Create wrapper script
    cat > "$binary" << EOF
#!/usr/bin/env bash
set -o xtrace
exec valgrind --gen-suppressions=all --quiet --error-exitcode=1 --suppressions=${SCRIPT_DIR}/contrib/valgrind.supp "${binary}_orig" "\$@"
EOF

    # Make wrapper executable
    chmod +x "$binary"
done

echo "Valgrind wrapping complete!"
