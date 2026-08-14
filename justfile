image := "bitcoin-core:latest"

[private]
default:
    just --list

# Develop using the devShell
develop:
    nix develop

# Build the docker image
build:
    nix build

# Load the image into the docker store
load: build
    docker load --input result

# Run the docker image
run: load
    docker run --rm {{ image }}

