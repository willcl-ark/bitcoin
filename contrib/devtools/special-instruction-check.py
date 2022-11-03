#!/usr/bin/env python3

"""
Check that generated out files which use special compilation units do not
contain any disallowed sections. See #18553 for additional context.

Special instructions include:

    SSE42
    SSE41
    AVX
    AVX2
    SHANI

Disallowed sections include:

    .text.startup

Example usage:

    find ./ -regex ".*\(sse41\|sse42\|avx\|avx2\|shani\).*\.o" | xargs python contrib/devtools/special-instruction-check.py

    python3 contrib/devtools/special-instruction-check.py --find

"""
import argparse
import glob
import os
import re
import sys

import lief


parser = argparse.ArgumentParser()
parser.add_argument("--find", action="store_true", default=False, help="search for si files using glob")
known_args, unknown_args = parser.parse_known_args()

if known_args.find:
    # Perform the search over file names
    cwd = os.getcwd()
    # Compile the regex of special instructions to search for
    pattern = re.compile(r".*(SSE42|SSE41|AVX|AVX2|SHANI).*", re.IGNORECASE)

    files = [file for file in glob.glob(f"{cwd}/**/*.o", recursive=True) if pattern.search(file)]
    # Warn over stderr if no files found, but don't fail. This could be a correct
    # success, or it could be that glob did not find the files (e.g. run from wrong
    # root directory)
    if not files:
        print(f"{__file__}: no special instruction *.o files found in {cwd}", file=sys.stderr)
        sys.exit(0)
    else:
        print(f"{__file__}: checking for disallowed sections in {', '.join(files)}", file=sys.stdout)
    print("Searching for something...")
else:
    # Use files passed in as args
    files = [file for file in unknown_args]

DISALLOWED_SECTIONS = [
    ".text.startup",
]

# Parse files for disallowed sections with lief
error = False
for file in files:
    out_file = lief.parse(file)
    for section in DISALLOWED_SECTIONS:
        if out_file.has_section(section):
            print(f"{__file__}: ERROR {file} contains disallowed section {section}", file=sys.stderr)
            error = True

sys.exit(1) if error else sys.exit(0)
