#!/usr/bin/env python3
"""
Check that generated out files which use special compilation units do not
contain any disallowed sections. See #18553 for additional context.

Example usage:
    python3 contrib/devtools/special-instruction-check.py
"""
from pathlib import Path
import re
import sys
from typing import List, Set

import lief  # type: ignore

# Configuration
SPECIAL_INSTRUCTIONS = {
    "SSE42",
    "SSE41",
    "AVX",
    "AVX2",
    "SHANI"
}

DISALLOWED_SECTIONS = {
    ".text.startup",
}

def find_special_instruction_files(root_dir: Path) -> List[Path]:
    """Find all object files containing special instruction patterns."""
    pattern = re.compile(
        "|".join(f"({instr})" for instr in SPECIAL_INSTRUCTIONS),
        re.IGNORECASE
    )
    return [
        file for file in root_dir.rglob("*.o")
        if pattern.search(file.name)
    ]

def check_file_sections(file_path: Path) -> Set[str]:
    """
    Check a binary file for disallowed sections.
    Returns set of found disallowed sections.
    """
    binary = lief.parse(str(file_path))
    if not binary:
        raise ValueError(f"Failed to parse {file_path}")

    file_sections = {section.name for section in binary.sections}
    return file_sections & DISALLOWED_SECTIONS

def main() -> int:
    print(f"{__file__}: Beginning special instruction check")
    root_dir = Path.cwd()
    files = find_special_instruction_files(root_dir)

    if not files:
        print(f"ERROR: No special instruction *.o files found in {root_dir}",
              file=sys.stderr)
        return 1


    print(f"Checking for the following disallowed sections: {DISALLOWED_SECTIONS}")
    print("Checking the following special instruction files:",
          ", ".join(str(f) for f in files),
          file=sys.stdout)

    has_error = False
    for file in files:
        try:
            bad_sections = check_file_sections(file)
            if bad_sections:
                print(f"ERROR: {file} contains disallowed sections: "
                      f"{', '.join(bad_sections)}",
                      file=sys.stderr)
                has_error = True
            else:
                print(f"OK: {file} does not contain disallowed sections")
        except ValueError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            has_error = True

    if has_error:
        print("ERROR: special instruction check did not complete successfully")
        return 1

    print("SUCCESS: special instruction check completed successfully")
    return 0

if __name__ == "__main__":
    sys.exit(main())
