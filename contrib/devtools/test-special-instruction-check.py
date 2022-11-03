#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test script for special-instruction-check.py
"""
import os
import subprocess
import unittest
from typing import List

import lief

from utils import determine_wellknown_cmd


def link(cc: List[str], source, executable, options):
    # This should behave the same as AC_TRY_LINK, so arrange well-known flags
    # in the same order as autoconf would.
    #
    # See the definitions for ac_link in autoconf's lib/autoconf/c.m4 file for
    # reference.
    env_flags: List[str] = []
    for var in ["CFLAGS", "CPPFLAGS", "LDFLAGS"]:
        env_flags += filter(None, os.environ.get(var, "").split(" "))

    subprocess.run([*cc, source, "-o", executable] + env_flags + options, check=True)


def call_si_check(source, executable):
    p = subprocess.run(
        [os.path.join(os.path.dirname(__file__), "special-instruction-check.py"), executable],
        stdout=subprocess.PIPE,
        universal_newlines=True,
    )
    os.remove(source)
    os.remove(executable)
    return p.returncode, p.stdout.rstrip()


class TestSymbolChecks(unittest.TestCase):
    def test_ELF_success(self):
        source = "sicheck.c"
        out_file = "sicheck.o"
        cc = determine_wellknown_cmd("CC", "gcc")

        with open(source, "w", encoding="utf8") as f:
            f.write(
                """
                #define _GNU_SOURCE
                #include <math.h>

                double nextup(double x);

                int main()
                {
                    nextup(3.14);
                    return 0;
                }
        """
            )

        link(cc, source, out_file, ["-c"])
        # Return code of 0 indicating success
        self.assertEqual(
            call_si_check(source, out_file)[0],
            0,
        )

    def test_ELF_failure(self):
        source = "sicheck.c"
        out_file = "sicheck.o"
        cc = determine_wellknown_cmd("CC", "gcc")

        with open(source, "w", encoding="utf8") as f:
            f.write(
                """
                #define _GNU_SOURCE
                #include <math.h>

                double nextup(double x);

                int main()
                {
                    nextup(3.14);
                    return 0;
                }
        """
            )

        link(cc, source, out_file, ["-c"])
        binary = lief.parse(out_file)

        # Add disallowed section .text.startup
        new_sec = lief.ELF.Section()
        new_sec.name = ".text.startup"
        new_sec.content = [0, 1, 2, 3, 4, 5, 6, 7]
        new_sec.size = 8
        new_sec.alignment = 4
        new_sec.virtual_address = 1000
        binary.add(new_sec, False)
        binary.write(out_file)

        # Return code of 1 indicating failure
        self.assertEqual(
            call_si_check(source, out_file)[0],
            1,
        )


if __name__ == "__main__":
    unittest.main()
