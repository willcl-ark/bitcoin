#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

import argparse
import os
import resource
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent / "test"))
from download_utils import download_script_assets


BUILD_DIR = Path("/home/build")
DEPENDS_DIR = Path("/home/depends")
INSTALL_DIR = Path("/home/install")
QA_ASSETS_DIR = Path("/home/qa-assets")
TEST_HOME_DIR = Path("/home/test_home")
TEST_RUNNER_DIR = Path("/home/test_runner")
CCACHE_WRAPPER_DIR = Path("/home/ccache-wrappers")


def run(cmd, **kwargs):
    print("+ " + shlex.join([str(arg) for arg in cmd]), flush=True)
    kwargs.setdefault("check", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


def jobs():
    return str(os.cpu_count() or 1)


def timeout_factor():
    return int(os.environ.get("TEST_RUNNER_TIMEOUT_FACTOR", "8"))


def bsd():
    for name in ("freebsd", "netbsd", "openbsd"):
        if sys.platform.startswith(name):
            return name
    sys.exit(f"Unsupported platform: {sys.platform}")


def host():
    guess = run(
        ["./depends/config.guess"], capture_output=True, text=True
    ).stdout.strip()
    result = run(
        ["./depends/config.sub", guess], capture_output=True, text=True
    ).stdout.strip()
    version = os.environ.get("BSD_VERSION")
    if not version:
        sys.exit("BSD_VERSION is not set")
    expected = f"x86_64-unknown-{bsd()}{version}"
    if result != expected:
        sys.exit(f"Unexpected BSD depends HOST: expected {expected}, got {result}")
    return result


def depends_lib_dir():
    return DEPENDS_DIR / host() / "lib"


def cache_path(name):
    value = os.environ.get(name)
    if not value:
        sys.exit(f"{name} is not set")
    return value


def ccache():
    if bsd() == "netbsd":
        return Path("/usr/pkg/bin/ccache")
    return Path("/usr/local/bin/ccache")


def prepare_ccache_dirs():
    Path(cache_path("CCACHE_DIR")).mkdir(parents=True, exist_ok=True)
    Path(cache_path("CCACHE_TEMPDIR")).mkdir(parents=True, exist_ok=True)


def ccache_env():
    prepare_ccache_dirs()
    compilers = ("gcc", "g++") if bsd() == "netbsd" else ("clang", "clang++")
    CCACHE_WRAPPER_DIR.mkdir(parents=True, exist_ok=True)
    for compiler in compilers:
        wrapper = CCACHE_WRAPPER_DIR / compiler
        wrapper.unlink(missing_ok=True)
        wrapper.symlink_to(ccache())

    compiler_dir = Path("/usr/pkg/gcc15/bin") if bsd() == "netbsd" else Path("/usr/bin")
    env = os.environ.copy()
    env["CCACHE_PATH"] = str(compiler_dir)
    env["PATH"] = os.pathsep.join(
        [str(CCACHE_WRAPPER_DIR), str(compiler_dir), env["PATH"]]
    )
    return env


def reset_ccache_stats():
    prepare_ccache_dirs()
    run([str(ccache()), "--zero-stats"])


def set_openbsd_limit(resource_type, soft_limit):
    if bsd() != "openbsd":
        return
    _, hard_limit = resource.getrlimit(resource_type)
    resource.setrlimit(resource_type, (soft_limit, hard_limit))


def build_depends():
    set_openbsd_limit(resource.RLIMIT_DATA, 3_000_000 * 1024)
    reset_ccache_stats()
    depends_host = host()
    options = []
    if bsd() in ("netbsd", "openbsd"):
        options.append("NO_QT=1")
    if bsd() in ("freebsd", "openbsd"):
        options.extend(["build_CC=clang", "build_CXX=clang++"])
    elif bsd() == "netbsd":
        options.extend(
            [
                "build_CC=gcc",
                "build_CXX=g++",
                "CC=gcc",
                "CXX=g++",
            ]
        )
    run(
        [
            "gmake",
            "-C",
            "depends",
            "-j",
            jobs(),
            f"HOST={depends_host}",
            f"BASE_CACHE={cache_path('BASE_CACHE')}",
            "LOG=1",
            f"SOURCES_PATH={cache_path('SOURCES_PATH')}",
            f"WORK_PATH={DEPENDS_DIR / 'work'}",
            f"x86_64_{bsd()}_prefix={DEPENDS_DIR / depends_host}",
            *options,
        ],
        env=ccache_env(),
    )


def configure():
    prepare_ccache_dirs()
    options = []
    env = None
    if bsd() == "freebsd":
        options.append("-DCMAKE_LINKER_TYPE=LLD")
    elif bsd() == "netbsd":
        env = os.environ.copy()
        env["PATH"] = os.pathsep.join(["/usr/pkg/gcc15/bin", env["PATH"]])
        options.append("-DBUILD_GUI=OFF")
    else:
        options.append("-DBUILD_GUI=OFF")
    run(
        [
            "cmake",
            "--preset=dev-mode",
            "-S",
            ".",
            "-B",
            str(BUILD_DIR),
            "--toolchain",
            str(DEPENDS_DIR / host() / "toolchain.cmake"),
            f"-DCCACHE_EXECUTABLE={ccache()}",
            "-DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
            f"-DCMAKE_INSTALL_PREFIX={INSTALL_DIR}",
            "-DREDUCE_EXPORTS=ON",
            "-DWITH_CCACHE=ON",
            "-DWITH_USDT=OFF",
            *options,
        ],
        env=env,
    )


def build():
    set_openbsd_limit(resource.RLIMIT_DATA, 3_000_000 * 1024)
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
    run([str(ccache()), "--show-stats", "--verbose"])


def check_bitcoind():
    bitcoind = BUILD_DIR / "bin" / "bitcoind"
    run(["ls", "-l", str(bitcoind)])
    run(["file", str(bitcoind)])
    run(["ldd", str(bitcoind)])
    run([str(bitcoind), "-version"])


def prepare_tests():
    env = os.environ.copy()
    pip_options = []
    if bsd() == "openbsd":
        env["LDCXXSHARED"] = "c++ -pthread -shared -fPIC"
        pip_options = ["--break-system-packages", "-C", "force-bundled-libcapnp=True"]
    elif bsd() == "netbsd":
        env["CXXFLAGS"] = "-DKJ_NO_EXCEPTIONS=0"
    run(
        [
            sys.executable,
            "-m",
            "pip",
            "install",
            "pycapnp",
            *pip_options,
        ],
        env=env,
    )

    TEST_HOME_DIR.mkdir(parents=True, exist_ok=True)
    datadir = TEST_HOME_DIR / ".bitcoin"
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
    env["HOME"] = str(TEST_HOME_DIR)
    env["LD_LIBRARY_PATH"] = str(depends_lib_dir())
    env["CTEST_OUTPUT_ON_FAILURE"] = "ON"
    if bsd() == "netbsd":
        env["TMPDIR"] = str(TEST_RUNNER_DIR)
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
    set_openbsd_limit(resource.RLIMIT_NOFILE, 1024)
    env = os.environ.copy()
    env["HOME"] = str(TEST_HOME_DIR)
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
    # FreeBSD's lsof-based tests can fail when run concurrently. NetBSD's
    # p2p_invalid_messages can exhaust memory. interface_rest can hang on
    # OpenBSD, so give its separate run twice the usual timeout.
    separate_tests = {
        "freebsd": (["feature_bind_extra", "rpc_bind"], "1", 1),
        "netbsd": (["p2p_invalid_messages"], None, 1),
        "openbsd": (["interface_rest"], None, 2),
    }
    test_names, separate_jobs, timeout_multiplier = separate_tests[bsd()]
    # Reindex is currently broken across BSD; see bitcoin/bitcoin#33128.
    excludes = ["feature_reindex_init", *test_names]
    run(
        [
            sys.executable,
            str(runner),
            "-j",
            jobs(),
            f"--timeout-factor={timeout_factor()}",
            *[arg for test in excludes for arg in ("--exclude", test)],
            *common_args,
        ],
        env=env,
    )
    separate_jobs_args = ["-j", separate_jobs] if separate_jobs else []
    run(
        [
            sys.executable,
            str(runner),
            *test_names,
            *separate_jobs_args,
            f"--timeout-factor={timeout_factor() * timeout_multiplier}",
            *common_args,
        ],
        env=env,
    )


def main():
    parser = argparse.ArgumentParser(description="Utility to run BSD CI steps.")
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
