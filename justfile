# Just file for Bitcoin CI jobs
#
# This file provides recipes for common CI operations.
# Install Just from https://just.systems/man/en/

default:
    just --list

# Show available jobs
jobs:
    @find ./ci/test/config -name "*.env" -type f | sort | sed 's|config/||g' | sed 's|\.env||g'

# Build all Docker images
build-all:
    for job in `just jobs`; do just build $$job; done

# Build a specific Docker image
build JOB:
    docker buildx bake --file ./ci/docker-bake.hcl --file ./ci/test/config/{{JOB}}.env {{JOB}}

# Run a job in Docker
run JOB *ARGS: (build JOB)
    MAKEJOBS=-j{{ num_cpus()}} ./ci/test/scripts/runner.sh --job {{JOB}} -- {{ARGS}}

# Run a job on the host system
run-host JOB *ARGS:
    MAKEJOBS=-j{{ num_cpus()}} ./ci/test/scripts/runner.sh --job {{JOB}} --host -- {{ARGS}}

# Clean up Docker volumes for a specific job
clean JOB:
    -docker volume rm bitcoin-ci-{{JOB}}-ccache
    -docker volume rm bitcoin-ci-{{JOB}}-depends
    -docker volume rm bitcoin-ci-{{JOB}}-depends-sources
    -docker volume rm bitcoin-ci-{{JOB}}-previous-releases

# Clean up all Docker resources related to CI
clean-all:
    -docker volume rm $(docker volume ls -q | grep bitcoin-ci-)
    -docker image rm $(docker image ls -q bitcoin-ci:)

# List available Docker images
list-images:
    docker image ls bitcoin-ci:*

# List available Docker volumes
list-volumes:
    docker volume ls | grep bitcoin-ci-

# Print the Docker Bake configuration for a job
print-config JOB:
    docker buildx bake --file ./ci/docker-bake.hcl --file ./ci/test/config/{{JOB}}.env --print {{JOB}}
