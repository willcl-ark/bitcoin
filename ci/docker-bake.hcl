# Docker Bake configuration for Bitcoin CI
#
# This file defines the targets for building the CI Docker images
# using Docker Buildx Bake.
#
# Usage:
#   docker buildx bake -f docker-bake.hcl -f config/arm.env --progress=plain arm

# Default group that builds all targets
group "default" {
  targets = ["native-asan"]
}

# Base image for Ubuntu 24.04
target "ubuntu2404-base" {
  context = "ci/test"
  dockerfile = "docker/Dockerfile.ubuntu2404-base"
  output = [{ type = "docker" }]
  tags = ["bitcoin-ci:ubuntu2404-base"]
  args = {
    DEBIAN_FRONTEND = "noninteractive"
    MAKEJOBS = "${MAKEJOBS}"
  }
}

target "native-asan" {
  inherits = ["ubuntu2404-base"]
  dockerfile = "docker/Dockerfile.native-asan"
  tags = ["bitcoin-ci:native-asan"]
  args = {
    APT_LLVM_V = "${APT_LLVM_V}"
    PACKAGES = "${PACKAGES}"
    BITCOIN_CONFIG = "${BITCOIN_CONFIG}"
    INSTALL_BCC_TRACING_TOOLS = "${INSTALL_BCC_TRACING_TOOLS}"
  }
}

################################################################################
# Dummy global variables
################################################################################
# These are evaluated for all targets so we add them here to avoid having to
# add them to all *.env files
variable "MAKEJOBS" {
  default = "-j4"
}

variable "APT_LLVM_V" {
  default = ""
}

variable "BITCOIN_CONFIG" {
  default = ""
}

variable "CI_CONTAINER_CAP" {
  default = ""
}

variable "CI_IMAGE_NAME_TAG" {
  default = ""
}

variable "CI_IMAGE_PLATFORM" {
  default = ""
}

variable "CONTAINER_NAME" {
  default = ""
}

variable "DPKG_ADD_ARCH" {
  default = ""
}

variable "GOAL" {
  default = ""
}

variable "HOST" {
  default = ""
}

variable "INSTALL_BCC_TRACING_TOOLS" {
  default = ""
}

variable "NO_DEPENDS" {
  default = ""
}

variable "PACKAGES" {
  default = ""
}

variable "RUN_FUNCTIONAL_TESTS" {
  default = ""
}

variable "RUN_FUZZ_TESTS" {
  default = ""
}

variable "RUN_UNIT_TESTS" {
  default = ""
}

variable "TEST_RUNNER_TIMEOUT_FACTOR" {
  default = ""
}

variable "USE_BUSY_BOX" {
  default = ""
}
