#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent / "test"))
from download_utils import download_script_assets


BUILD_DIR = Path("/home/build")
INSTALL_DIR = Path("/home/install")
QA_ASSETS_DIR = Path("/home/qa-assets")
TEST_RUNNER_DIR = Path("/home/test_runner")


def run(cmd, **kwargs):
    print("+ " + shlex.join([str(arg) for arg in cmd]), flush=True)
    kwargs.setdefault("check", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


def jobs():
    return str(os.process_cpu_count() or 1)


def timeout_factor():
    return int(os.environ.get("TEST_RUNNER_TIMEOUT_FACTOR", "8"))


def host():
    guess = run(
        ["./depends/config.guess"], capture_output=True, text=True
    ).stdout.strip()
    result = run(
        ["./depends/config.sub", guess], capture_output=True, text=True
    ).stdout.strip()
    expected = f"x86_64-unknown-openbsd{os.environ.get('OPENBSD_VERSION', '7.9')}"
    if result != expected:
        sys.exit(f"Unexpected OpenBSD depends HOST: expected {expected}, got {result}")
    return result


def depends_lib_dir():
    return Path.cwd() / "depends" / host() / "lib"


def build_depends():
    run(
        [
            "gmake",
            "-C",
            "depends",
            "-j",
            jobs(),
            f"HOST={host()}",
            "NO_QT=1",
            "build_CC=clang",
            "build_CXX=clang++",
            "LOG=1",
        ]
    )


def configure():
    run(
        [
            "cmake",
            "--preset=dev-mode",
            "-S",
            ".",
            "-B",
            str(BUILD_DIR),
            "--toolchain",
            str(Path.cwd() / "depends" / host() / "toolchain.cmake"),
            "-DBUILD_GUI=OFF",
            "-DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
            f"-DCMAKE_INSTALL_PREFIX={INSTALL_DIR}",
            "-DREDUCE_EXPORTS=ON",
            "-DWITH_USDT=OFF",
        ]
    )


def build():
    run(
        [
            "cmake",
            "--build",
            str(BUILD_DIR),
            "-j",
            jobs(),
            "--target",
            "all",
            "install",
        ]
    )


def check_bitcoind():
    bitcoind = BUILD_DIR / "bin" / "bitcoind"
    run(["ls", "-l", str(bitcoind)])
    run(["file", str(bitcoind)])
    run(["ldd", str(bitcoind)])
    run([str(bitcoind), "-version"])


def prepare_tests():
    env = os.environ.copy()
    env["LDCXXSHARED"] = "c++ -pthread -shared -fPIC"
    run(
        [
            "pip3",
            "install",
            "--break-system-packages",
            "pycapnp",
            "-C",
            "force-bundled-libcapnp=True",
        ],
        env=env,
    )

    datadir = Path.home() / ".bitcoin"
    if datadir.exists():
        sys.exit(f"Default datadir path already exists: {datadir}")
    datadir.write_text("")

    unit_test_data = QA_ASSETS_DIR / "unit_test_data"
    unit_test_data.mkdir(parents=True, exist_ok=True)
    TEST_RUNNER_DIR.mkdir(parents=True, exist_ok=True)
    download_script_assets(unit_test_data)


def run_unit_tests():
    env = os.environ.copy()
    env["DIR_UNIT_TEST_DATA"] = str(QA_ASSETS_DIR / "unit_test_data")
    env["LD_LIBRARY_PATH"] = str(depends_lib_dir())
    env["CTEST_OUTPUT_ON_FAILURE"] = "ON"
    run(
        [
            "ctest",
            "--test-dir",
            str(BUILD_DIR),
            "--stop-on-failure",
            "-j",
            jobs(),
            "--timeout",
            str(timeout_factor() * 60),
        ],
        env=env,
    )


def run_functional_tests():
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(depends_lib_dir())
    common_args = [
        "--tmpdirprefix",
        f"{TEST_RUNNER_DIR}/",
        "--ansi",
        "--combinedlogslen=99999999",
        "--quiet",
        "--failfast",
    ]
    runner = BUILD_DIR / "test" / "functional" / "test_runner.py"
    run(
        [
            str(runner),
            "-j",
            jobs(),
            f"--timeout-factor={timeout_factor()}",
            "--exclude",
            "feature_reindex_init",
            "--exclude",
            "interface_rest",
            *common_args,
        ],
        env=env,
    )
    run(
        [
            str(runner),
            "interface_rest",
            f"--timeout-factor={timeout_factor() * 2}",
            *common_args,
        ],
        env=env,
    )


def main():
    parser = argparse.ArgumentParser(description="Utility to run OpenBSD CI steps.")
    steps = {
        "build_depends": build_depends,
        "configure": configure,
        "build": build,
        "check_bitcoind": check_bitcoind,
        "prepare_tests": prepare_tests,
        "run_unit_tests": run_unit_tests,
        "run_functional_tests": run_functional_tests,
    }
    parser.add_argument("step", choices=steps, help="CI step to perform.")
    args = parser.parse_args()

    steps[args.step]()


if __name__ == "__main__":
    main()
