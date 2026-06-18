#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


DOCKER_CACHE_GROUP_ENVS = {
    "repository-setup": [
        "APPEND_APT_SOURCES_LIST",
        "APT_LLVM_V",
        "CI_OS_NAME",
        "CI_RETRY_EXE",
        "DEBIAN_FRONTEND",
        "DPKG_ADD_ARCH",
    ],
    "system-packages": [
        "APT_LLVM_V",
        "CI_BASE_PACKAGES",
        "CI_OS_NAME",
        "CI_RETRY_EXE",
        "DEBIAN_FRONTEND",
        "PACKAGES",
    ],
    "python-packages": [
        "CI_RETRY_EXE",
        "PIP_PACKAGES",
    ],
    "tool-builds": [
        "CI_RETRY_EXE",
        "IWYU_LLVM_V",
        "RUN_IWYU",
        "USE_INSTRUMENTED_LIBCPP",
    ],
}


def run(cmd, **kwargs):
    print("+ " + shlex.join(cmd), flush=True)
    kwargs.setdefault("check", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


def copy_context_file(source_root, context_dir, relative_path):
    source_path = source_root / relative_path
    destination_path = context_dir / relative_path
    destination_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_path, destination_path)


def write_cache_group_env(context_dir, group, variables):
    env_path = context_dir / "ci" / "test" / "cache-env" / f"{group}.env"
    env_path.parent.mkdir(parents=True, exist_ok=True)
    with env_path.open("w", encoding="utf8") as file:
        for key in variables:
            file.write(f"export {key}={shlex.quote(os.environ.get(key, ''))}\n")


def generate_docker_build_context(context_dir):
    source_root = Path(os.environ["BASE_READ_ONLY_DIR"])
    context_dir.mkdir(parents=True, exist_ok=True)

    for relative_path in [
        Path("ci/retry/retry"),
        Path("ci/test/01_base_install.sh"),
        Path("ci/test/01_iwyu.patch"),
        Path("ci/test/02_iwyu_hash.patch"),
        Path("ci/test_imagefile"),
    ]:
        copy_context_file(source_root, context_dir, relative_path)

    for group, variables in DOCKER_CACHE_GROUP_ENVS.items():
        write_cache_group_env(context_dir, group, variables)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generate-docker-context-only", type=Path)
    args = parser.parse_args()

    print("Export only allowed settings:")
    settings = run(
        ["bash", "-c", "grep export ./ci/test/00_setup_env*.sh"],
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.splitlines()
    settings = set(l.split("=")[0].split("export ")[1] for l in settings)
    # Add "hidden" settings, which are never exported, manually. Otherwise,
    # they will not be passed on.
    settings.update([
        "BASE_BUILD_DIR",
        "CI_FAILFAST_TEST_LEAVE_DANGLING",
    ])

    # Append $USER to /tmp/env to support multi-user systems and $CONTAINER_NAME
    # to allow support starting multiple runs simultaneously by the same user.
    env_file = "/tmp/env-{u}-{c}".format(
        u=os.environ["USER"],
        c=os.environ["CONTAINER_NAME"],
    )
    with open(env_file, "w") as file:
        for k, v in os.environ.items():
            if k in settings:
                file.write(f"{k}={v}\n")
    run(["cat", env_file])

    if args.generate_docker_context_only:
        generate_docker_build_context(args.generate_docker_context_only)
        print(f"Generated Docker build context at {args.generate_docker_context_only}")
        return

    if os.getenv("DANGER_RUN_CI_ON_HOST"):
        print("Running on host system without docker wrapper")
        print("Create missing folders")
        for create_dir in [
                os.environ["CCACHE_DIR"],
                os.environ["PREVIOUS_RELEASES_DIR"],
        ]:
            Path(create_dir).mkdir(parents=True, exist_ok=True)

        # Modify PATH to prepend the retry script, needed for CI_RETRY_EXE
        os.environ["PATH"] = f"{os.environ['BASE_ROOT_DIR']}/ci/retry:{os.environ['PATH']}"
    else:
        CI_IMAGE_LABEL = "bitcoin-ci-test"
        docker_build_context = tempfile.TemporaryDirectory(prefix="bitcoin-ci-build-context-")
        build_context_path = Path(docker_build_context.name)
        generate_docker_build_context(build_context_path)

        # Use buildx unconditionally
        # Using buildx is required to properly load the correct driver, for use with registry caching. Neither build, nor BUILDKIT=1 currently do this properly
        cmd_build = ["docker", "buildx", "build"]
        cmd_build += [
            f"--file={build_context_path}/ci/test_imagefile",
            f"--build-arg=CI_IMAGE_NAME_TAG={os.environ['CI_IMAGE_NAME_TAG']}",
            f"--platform={os.environ['CI_IMAGE_PLATFORM']}",
            f"--label={CI_IMAGE_LABEL}",
            f"--tag={os.environ['CONTAINER_NAME']}",
        ]
        cmd_build += shlex.split(os.getenv("DOCKER_BUILD_CACHE_ARG", ""))
        cmd_build += [str(build_context_path)]

        print(f"Building {os.environ['CONTAINER_NAME']} image tag to run in")
        if run(cmd_build, check=False).returncode != 0:
            print(f"Retry building {os.environ['CONTAINER_NAME']} image tag after failure")
            time.sleep(3)
            run(cmd_build)

        for suffix in ["ccache", "depends", "depends_sources", "previous_releases"]:
            run(["docker", "volume", "create", f"{os.environ['CONTAINER_NAME']}_{suffix}"], check=False)

        CI_CCACHE_MOUNT = f"type=volume,src={os.environ['CONTAINER_NAME']}_ccache,dst={os.environ['CCACHE_DIR']}"
        CI_DEPENDS_MOUNT = f"type=volume,src={os.environ['CONTAINER_NAME']}_depends,dst={os.environ['DEPENDS_DIR']}/built"
        CI_DEPENDS_SOURCES_MOUNT = f"type=volume,src={os.environ['CONTAINER_NAME']}_depends_sources,dst={os.environ['DEPENDS_DIR']}/sources"
        CI_PREVIOUS_RELEASES_MOUNT = f"type=volume,src={os.environ['CONTAINER_NAME']}_previous_releases,dst={os.environ['PREVIOUS_RELEASES_DIR']}"
        CI_BUILD_MOUNT = []

        if os.getenv("DANGER_CI_ON_HOST_FOLDERS"):
            # ensure the directories exist
            for create_dir in [
                    os.environ["CCACHE_DIR"],
                    f"{os.environ['DEPENDS_DIR']}/built",
                    f"{os.environ['DEPENDS_DIR']}/sources",
                    os.environ["PREVIOUS_RELEASES_DIR"],
                    os.environ["BASE_BUILD_DIR"],  # Unset by default, must be defined externally
            ]:
                Path(create_dir).mkdir(parents=True, exist_ok=True)

            CI_CCACHE_MOUNT = f"type=bind,src={os.environ['CCACHE_DIR']},dst={os.environ['CCACHE_DIR']}"
            CI_DEPENDS_MOUNT = f"type=bind,src={os.environ['DEPENDS_DIR']}/built,dst={os.environ['DEPENDS_DIR']}/built"
            CI_DEPENDS_SOURCES_MOUNT = f"type=bind,src={os.environ['DEPENDS_DIR']}/sources,dst={os.environ['DEPENDS_DIR']}/sources"
            CI_PREVIOUS_RELEASES_MOUNT = f"type=bind,src={os.environ['PREVIOUS_RELEASES_DIR']},dst={os.environ['PREVIOUS_RELEASES_DIR']}"
            CI_BUILD_MOUNT = [f"--mount=type=bind,src={os.environ['BASE_BUILD_DIR']},dst={os.environ['BASE_BUILD_DIR']}"]

        if os.getenv("DANGER_CI_ON_HOST_CCACHE_FOLDER"):
            if not os.path.isdir(os.environ["CCACHE_DIR"]):
                print(f"Error: Directory '{os.environ['CCACHE_DIR']}' must be created in advance.")
                sys.exit(1)
            CI_CCACHE_MOUNT = f"type=bind,src={os.environ['CCACHE_DIR']},dst={os.environ['CCACHE_DIR']}"

        run(["docker", "network", "create", "--ipv6", "--subnet", "1111:1111::/112", "ci-ip6net"], check=False)
        run(["docker", "network", "create", "--subnet", "1.1.1.0/24", "ci-ip4net"], check=False)

        if os.getenv("RESTART_CI_DOCKER_BEFORE_RUN"):
            print("Restart docker before run to stop and clear all containers started with --rm")
            run(["podman", "container", "rm", "--force", "--all"])  # Similar to "systemctl restart docker"

            # Still prune everything in case the filtered pruning doesn't work, or if labels were not set
            # on a previous run. Belt and suspenders approach, should be fine to remove in the future.
            # Prune images used by --external containers (e.g. build containers) when
            # using podman.
            print("Prune all dangling images")
            run(["podman", "image", "prune", "--force", "--external"])

        print(f"Prune all dangling {CI_IMAGE_LABEL} images")
        # When detecting podman-docker, `--external` should be added.
        run(["docker", "image", "prune", "--force", "--filter", f"label={CI_IMAGE_LABEL}"])

        cmd_run = ["docker", "run", "--rm", "--interactive", "--detach", "--tty"]
        cmd_run += [
            "--cap-add=LINUX_IMMUTABLE",
            *shlex.split(os.getenv("CI_CONTAINER_CAP", "")),
            f"--mount=type=bind,src={os.environ['BASE_READ_ONLY_DIR']},dst={os.environ['BASE_READ_ONLY_DIR']},readonly",
            f"--mount={CI_CCACHE_MOUNT}",
            f"--mount={CI_DEPENDS_MOUNT}",
            f"--mount={CI_DEPENDS_SOURCES_MOUNT}",
            f"--mount={CI_PREVIOUS_RELEASES_MOUNT}",
            *CI_BUILD_MOUNT,
            f"--env-file={env_file}",
            f"--name={os.environ['CONTAINER_NAME']}",
            "--network=ci-ip6net",
            "--ip6=1111:1111::5", # Used by some of the tests, don't change it just here (keep them in sync).
            f"--platform={os.environ['CI_IMAGE_PLATFORM']}",
            os.environ["CONTAINER_NAME"],
        ]

        container_id = run(
            cmd_run,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout.strip()

        run(["docker", "network", "connect", "--ip=1.1.1.5", "ci-ip4net", container_id]) # The IP address is used by some of the tests, don't change it just here (keep them in sync).

    def ci_exec(cmd_inner, **kwargs):
        if os.getenv("DANGER_RUN_CI_ON_HOST"):
            prefix = []
        else:
            prefix = [
                "docker",
                "exec",
                "--env",
                "DANGER_RUN_CI_ON_HOST=1",  # Safe to set *inside* the container
                container_id,
            ]

        return run([*prefix, *cmd_inner], **kwargs)

    # Normalize all folders to BASE_ROOT_DIR
    ci_exec([
        "rsync",
        "--recursive",
        "--perms",
        "--stats",
        "--human-readable",
        f"{os.environ['BASE_READ_ONLY_DIR']}/",
        f"{os.environ['BASE_ROOT_DIR']}",
    ])
    if os.getenv("DANGER_RUN_CI_ON_HOST"):
        ci_exec([f"{os.environ['BASE_ROOT_DIR']}/ci/test/01_base_install.sh"])
    else:
        ci_exec([f"{os.environ['BASE_ROOT_DIR']}/ci/test/01_base_install.sh", "runtime-paths"])
        ci_exec([f"{os.environ['BASE_ROOT_DIR']}/ci/test/01_base_install.sh", "mark-done"])
    ci_exec([f"{os.environ['BASE_ROOT_DIR']}/ci/test/03_test_script.sh"])

    if not os.getenv("DANGER_RUN_CI_ON_HOST"):
        print("Stop and remove CI container by ID")
        run(["docker", "container", "kill", container_id])
        docker_build_context.cleanup()


if __name__ == "__main__":
    main()
