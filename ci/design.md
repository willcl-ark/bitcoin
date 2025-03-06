# Bitcoin CI System Redesign: Docker-First Approach

A possible design for transitioning to a docker-first approach while maintaining the ability to run on bare metal, focussing on simplicity, maintainability, and adhering to Docker best practices.

## Current Setup

The current system uses:
- Environment setup scripts (`00_setup_env_*.sh`) for different configurations
- A base installation script (`01_base_install.sh`)
- A container run script (`02_run_container.sh`)
- A test execution script (`03_test_script.sh`)
- Wrapper scripts for Valgrind and Wine
- A generic `test_imagefile` as a Dockerfile template

This setup dynamically builds Docker images with varying configurations based on environment variables.

## Design

### 1. Docker-First Structure

```
ci
 └──test/
    ├── docker/
    │   ├── docker-bake.hcl             # Main bake file for all targets
    │   ├── Dockerfile.base             # Base image with common dependencies
    │   ├── Dockerfile.arm              # Per-job Dockerfiles
    │   ├── Dockerfile.i686_multiprocess
    │   ├── Dockerfile.mac_cross
    │   └── ...
    ├── scripts/
    │   ├── runner.sh                   # Main script to run jobs
    │   ├── common.sh                   # Common functions
    │   ├── host-runner.sh              # For running on bare metal
    │   └── docker-runner.sh            # For running in Docker
    └── justfile                        # Command runner with recipes
```

### 2. Docker Bake File (docker-bake.hcl)

The `docker-bake.hcl` file will define:
- A base "builder" target with common dependencies
- Individual targets for each job configuration
- Variable definitions for customization
- Default values from current environment scripts

Example structure:
```hcl
variable "MAKEJOBS" {
  default = "-j8"
}

group "default" {
  targets = ["arm", "i686_multiprocess", "mac_cross", ...]
}

target "builder-base" {
  dockerfile = "Dockerfile.base"
  platforms = ["linux/amd64"]
  args = {
    DEBIAN_FRONTEND = "noninteractive"
    MAKEJOBS = "${MAKEJOBS}"
  }
}

target "arm" {
  inherits = ["builder-base"]
  dockerfile = "Dockerfile.arm"
  tags = ["bitcoin-ci:arm"]
  args = {
    HOST = "arm-linux-gnueabihf"
    PACKAGES = "python3-zmq g++-arm-linux-gnueabihf busybox libc6:armhf libstdc++6:armhf libfontconfig1:armhf libxcb1:armhf"
    BITCOIN_CONFIG = "-DREDUCE_EXPORTS=ON -DCMAKE_CXX_FLAGS='-Wno-psabi -Wno-error=maybe-uninitialized'"
  }
}

# Additional targets for other configurations...
```

### 3. Dockerfiles

#### Base Dockerfile (Dockerfile.base)

```dockerfile
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG MAKEJOBS=-j4

# Install common dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    pkgconf \
    curl \
    ca-certificates \
    ccache \
    python3 \
    rsync \
    git \
    procps \
    bison \
    e2fsprogs \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Create directory structure
RUN mkdir -p /ci_container_base/depends \
    /ci_container_base/ci/scratch/ccache \
    /ci_container_base/ci/scratch/out \
    /ci_container_base/prev_releases

# Set environment variables
ENV BASE_ROOT_DIR=/ci_container_base
ENV DEPENDS_DIR=/ci_container_base/depends
ENV BASE_SCRATCH_DIR=/ci_container_base/ci/scratch
ENV BINS_SCRATCH_DIR=/ci_container_base/ci/scratch/bins
ENV CCACHE_DIR=/ci_container_base/ci/scratch/ccache
ENV BASE_OUTDIR=/ci_container_base/ci/scratch/out
ENV PREVIOUS_RELEASES_DIR=/ci_container_base/prev_releases

# Copy retry script
COPY scripts/retry.sh /usr/bin/retry
RUN chmod +x /usr/bin/retry

WORKDIR /ci_container_base
```

#### Job-Specific Dockerfile (e.g., Dockerfile.arm)

```dockerfile
FROM bitcoin-ci:base

ARG HOST=arm-linux-gnueabihf
ARG PACKAGES="python3-zmq g++-arm-linux-gnueabihf busybox libc6:armhf libstdc++6:armhf libfontconfig1:armhf libxcb1:armhf"
ARG BITCOIN_CONFIG="-DREDUCE_EXPORTS=ON -DCMAKE_CXX_FLAGS='-Wno-psabi -Wno-error=maybe-uninitialized'"

# Add armhf architecture
RUN dpkg --add-architecture armhf

# Install job-specific packages
RUN apt-get update && apt-get install -y --no-install-recommends \
    ${PACKAGES} \
    && rm -rf /var/lib/apt/lists/*

# Set environment variables
ENV HOST=${HOST}
ENV BITCOIN_CONFIG=${BITCOIN_CONFIG}
ENV USE_BUSY_BOX=true
ENV RUN_UNIT_TESTS=true
ENV RUN_FUNCTIONAL_TESTS=false
ENV GOAL="install"

# Create working directory for mounting source
WORKDIR /bitcoin
```

### 4. Runner Script (runner.sh)

```bash
#!/usr/bin/env bash
#
# Main runner script for Bitcoin CI

set -eo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/common.sh"

# Parse arguments
JOB_NAME=""
HOST_MODE=false
EXTRA_ARGS=""

print_usage() {
    echo "Usage: $0 --job JOB_NAME [--host] [-- EXTRA_ARGS]"
    echo "  --job JOB_NAME   Specify the job to run (e.g., arm, mac_cross)"
    echo "  --host           Run on host system (not in Docker)"
    echo "  -- EXTRA_ARGS    Pass additional arguments to the job"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --job)
            JOB_NAME="$2"
            shift 2
            ;;
        --host)
            HOST_MODE=true
            shift
            ;;
        --)
            shift
            EXTRA_ARGS="$*"
            break
            ;;
        *)
            echo "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

if [[ -z "$JOB_NAME" ]]; then
    echo "Error: Job name not specified"
    print_usage
    exit 1
fi

# Set up environment
if [[ "$HOST_MODE" == true ]]; then
    "${SCRIPT_DIR}/host-runner.sh" "$JOB_NAME" $EXTRA_ARGS
else
    "${SCRIPT_DIR}/docker-runner.sh" "$JOB_NAME" $EXTRA_ARGS
fi
```

### 5. Docker Runner Script (docker-runner.sh)

```bash
#!/usr/bin/env bash
#
# Runner script for Bitcoin CI in Docker

set -eo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/common.sh"

JOB_NAME="$1"
shift
EXTRA_ARGS="$*"

# Check if image exists or build it
if ! docker image inspect "bitcoin-ci:${JOB_NAME}" &>/dev/null; then
    echo "Building image for ${JOB_NAME}..."
    docker buildx bake --file "${SCRIPT_DIR}/../docker/docker-bake.hcl" "${JOB_NAME}"
fi

# Create volumes if they don't exist
docker volume create "bitcoin-ci-${JOB_NAME}-ccache" 2>/dev/null || true
docker volume create "bitcoin-ci-${JOB_NAME}-depends" 2>/dev/null || true
docker volume create "bitcoin-ci-${JOB_NAME}-previous-releases" 2>/dev/null || true

# Get source directory (current directory or specified by argument)
SRC_DIR="$(pwd)"

# Run the container
docker run --rm -it \
    -v "${SRC_DIR}:/bitcoin:ro" \
    -v "bitcoin-ci-${JOB_NAME}-ccache:/ci_container_base/ci/scratch/ccache" \
    -v "bitcoin-ci-${JOB_NAME}-depends:/ci_container_base/depends/built" \
    -v "bitcoin-ci-${JOB_NAME}-previous-releases:/ci_container_base/prev_releases" \
    --name "bitcoin-ci-${JOB_NAME}" \
    "bitcoin-ci:${JOB_NAME}" \
    bash -c "cd /bitcoin && ./ci/test/03_test_script.sh ${EXTRA_ARGS}"
```

### 6. Host Runner Script (host-runner.sh)

```bash
#!/usr/bin/env bash
#
# Runner script for Bitcoin CI on bare metal

set -eo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${SCRIPT_DIR}/common.sh"

JOB_NAME="$1"
shift
EXTRA_ARGS="$*"

# Source the job-specific environment variables
if [[ -f "${SCRIPT_DIR}/../config/${JOB_NAME}.env" ]]; then
    source "${SCRIPT_DIR}/../config/${JOB_NAME}.env"
else
    echo "Error: Job configuration not found for ${JOB_NAME}"
    exit 1
fi

# Set the environment variables
export DANGER_RUN_CI_ON_HOST=1
export CCACHE_DIR="${PWD}/ci/scratch/ccache"
export DEPENDS_DIR="${PWD}/depends"
export PREVIOUS_RELEASES_DIR="${PWD}/prev_releases"

# Create directories
mkdir -p "${CCACHE_DIR}" "${DEPENDS_DIR}" "${PREVIOUS_RELEASES_DIR}"

# Run the test script
./ci/test/03_test_script.sh ${EXTRA_ARGS}
```

### 7. Command Runner (justfile)

```justfile
# Just file for Bitcoin CI jobs

default:
    @just --list

# Build all Docker images
build-all:
    docker buildx bake --file docker/docker-bake.hcl

# Build a specific Docker image
build JOB:
    docker buildx bake --file docker/docker-bake.hcl {{JOB}}

# Run a job in Docker
run JOB *ARGS:
    ./scripts/runner.sh --job {{JOB}} -- {{ARGS}}

# Run a job on the host system
run-host JOB *ARGS:
    ./scripts/runner.sh --job {{JOB}} --host -- {{ARGS}}

# Clean up Docker volumes
clean JOB:
    -docker volume rm bitcoin-ci-{{JOB}}-ccache
    -docker volume rm bitcoin-ci-{{JOB}}-depends
    -docker volume rm bitcoin-ci-{{JOB}}-previous-releases

# Clean up all Docker resources
clean-all:
    -docker volume rm $(docker volume ls -q | grep bitcoin-ci)
    -docker image rm $(docker image ls -q bitcoin-ci)
```

### 8. Config Files

For each job configuration, extract environment variables to a `.env` file that can be used by both Docker and host runners:

Example `config/arm.env`:
```bash
HOST=arm-linux-gnueabihf
PACKAGES="python3-zmq g++-arm-linux-gnueabihf busybox libc6:armhf libstdc++6:armhf libfontconfig1:armhf libxcb1:armhf"
CONTAINER_NAME=ci_arm_linux
CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:noble"
CI_IMAGE_PLATFORM="linux/arm64"
USE_BUSY_BOX=true
RUN_UNIT_TESTS=true
RUN_FUNCTIONAL_TESTS=false
GOAL="install"
BITCOIN_CONFIG="-DREDUCE_EXPORTS=ON -DCMAKE_CXX_FLAGS='-Wno-psabi -Wno-error=maybe-uninitialized'"
```

## Docker Best Practices

1. Multi-stage builds reduce image size
2. Better layer caching
3. Minimal base images: Start with Ubuntu/Debian and adding only necessary packages
4. Set environment variables in the Dockerfile
5. Using Docker volumes for persistent data
6. Optimized package installation to reduce layers
7. Use of .dockerignore to avoid unnecessary file copying

## Migration

1. First, create the base folder structure and scripts
2. Extract environment variables from existing scripts to config files
3. Create the base Dockerfile and test
4. Implement one job-specific Dockerfile as a prototype
5. Create the runner scripts and Just file
6. Test the prototype end-to-end
7. Implement remaining job-specific Dockerfiles
8. Set up CI to use the new Docker-based system
9. Update documentation

## Benefits

1. Each job has a dedicated Dockerfile
2. Base image and common components are reused
3. Dockerfiles explicitly define the environment
4. New job types can still be added with minimal changes
5. Environment variables are defined in a single place
6. Native Docker support: Leverages Docker features like bake and volumes
