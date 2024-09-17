#!/bin/bash

set -eu

echo ""
echo "Starting with macOS 15 (Sequoia), Apple has introduced stricter security measures."
echo "Applications that are not notarized, codesigned or downloaded from the App Store may be blocked from running by Gatekeeper."
echo ""
echo "Due to changes in macOS security policies, applications that are not codesigned or notarized my be blocked form running"
echo "Bitcoin Core binaries are distributed unsigned for security and transparency reasons."
echo ""
echo "This script helps you codesign the Bitcoin Core binaries yourself so that you can run them on your Mac."
echo ""

choose_directory() {
  read -r -p "Please specify the directory where the binaries are located (press Enter to use the current directory): " DIR
  DIR=${DIR:-$PWD}
}

choose_directory

if [ ! -d "$DIR" ]; then
  echo "Error: Directory '$DIR' does not exist."
  exit 1
fi

# Check if 'bin' directory exists within the specified directory, as it will
# for downloaded tarballs when running script from default location.
if [ -d "$DIR/bin" ]; then
  DIR="$DIR/bin"
fi

# Check for the presence of 'bitcoind' and 'bitcoin-cli' to make sure we're in
# the right place before continuing.
if [ ! -f "$DIR/bitcoind" ] || [ ! -f "$DIR/bitcoin-cli" ]; then
  echo "Error: 'bitcoind' and/or 'bitcoin-cli' not found in '$DIR'."
  echo "Please ensure you pass the correct directory."
  exit 1
fi

codesign --sign - "$DIR"/*

xattr -dr com.apple.quarantine "$DIR"

echo "The binaries have been codesigned successfully"
echo ""
echo "You should now be able to run the binaries by right-clicking and choosing 'Open' twice."
echo "Or by running them directly from your terminal."
exit 0
