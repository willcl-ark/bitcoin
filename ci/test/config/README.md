# CI Configuration Files

This directory contains configuration files for Bitcoin CI jobs. Each file defines environment variables used by both Docker builds and host runners.

## Creating a Job Configuration

To create a new job configuration:

1. Create a `.env` file named after your job (e.g., `arm.env` for the ARM job)
2. Define environment variables in KEY=VALUE format
3. These variables will be used by both Docker Bake and host runners

Example configuration file:

```
# Example CI job configuration
#
# This file defines environment variables for the Example CI job.
# Used with docker-bake.hcl: docker buildx bake -f docker-bake.hcl -f config/example.env example

HOST=example-linux-gnu
PACKAGES=python3-zmq gcc
CONTAINER_NAME=ci_example
CI_IMAGE_NAME_TAG=mirror.gcr.io/ubuntu:24.04
CI_IMAGE_PLATFORM=linux/amd64
RUN_UNIT_TESTS=true
RUN_FUNCTIONAL_TESTS=true
GOAL=install
BITCOIN_CONFIG=-DREDUCE_EXPORTS=ON
```

## Variables Reference

The following variables can be defined in a configuration file:

| Variable | Description | Default |
|----------|-------------|---------|
| APT_LLVM_V | LLVM version to install | "" |
| BITCOIN_CONFIG | CMake configuration options | "" |
| CCACHE_MAXSIZE | Maximum ccache size | 500M |
| CI_IMAGE_NAME_TAG | Base Docker image | mirror.gcr.io/ubuntu:24.04 |
| CI_IMAGE_PLATFORM | Platform for Docker image | linux/amd64 |
| CONTAINER_NAME | Name for the Docker container | ci_$job_name |
| DEP_OPTS | Dependencies options | "" |
| DPKG_ADD_ARCH | Additional architecture for Debian/Ubuntu | "" |
| GOAL | Build goal (e.g., install, deploy) | install |
| HOST | Target architecture | Current architecture |
| NO_DEPENDS | Skip building dependencies | "" |
| PACKAGES | Packages to install | "" |
| RUN_FUNCTIONAL_TESTS | Run functional tests | true |
| RUN_FUZZ_TESTS | Run fuzz tests | false |
| RUN_UNIT_TESTS | Run unit tests | true |
| TEST_RUNNER_TIMEOUT_FACTOR | Test runner timeout factor | 40 |
| USE_BUSY_BOX | Use BusyBox instead of GNU utilities | false |

## Using Configuration Files

These configuration files serve as the single source of truth for both Docker image building and host runners.

### With Docker Bake

```bash
# Build a specific job's Docker image
docker buildx bake -f docker-bake.hcl -f config/arm.env arm

# Print the resolved configuration
docker buildx bake -f docker-bake.hcl -f config/arm.env --print arm
```

### With Runner Scripts

```bash
# Run a job in Docker
./scripts/runner.sh --job arm

# Run a job on the host
./scripts/runner.sh --job arm --host
```

### With Just

```bash
# Build a specific job's Docker image
just build arm

# Run a job in Docker
just run arm

# Run a job on the host
just run-host arm
```

## Adding a New Job

To add a new job based on an existing one:

```bash
just new-job arm new_job
just new-dockerfile arm new_job
```

Then edit the files to customize them for the new job.
