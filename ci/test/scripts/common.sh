#!/usr/bin/env bash
#
# Common functions for Bitcoin CI scripts
#
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Log a message to stderr
log_info() {
  echo >&2 "INFO: $*"
}

# Log an error message to stderr
log_error() {
  echo >&2 "ERROR: $*"
}

# Log a warning message to stderr
log_warning() {
  echo >&2 "WARNING: $*"
}

# Check if a command exists
command_exists() {
  command -v "$1" &> /dev/null
}

# Check if running in a Docker container
is_in_container() {
  [ -f /.dockerenv ] || grep -q docker /proc/1/cgroup 2>/dev/null
}

# Get absolute path
get_abs_path() {
  local path="$1"
  if [[ "$path" != /* ]]; then
    path="$PWD/$path"
  fi
  echo "$path"
}

# Check if a job configuration exists
job_config_exists() {
  local job_name="$1"
  local config_dir="$2"

  [[ -f "${config_dir}/${job_name}.env" ]]
}

# Source a job configuration file
source_job_config() {
  local job_name="$1"
  local config_dir="$2"

  if [[ -f "${config_dir}/${job_name}.env" ]]; then
    # shellcheck disable=SC1090
    source "${config_dir}/${job_name}.env"
    return 0
  else
    log_error "Job configuration not found: ${config_dir}/${job_name}.env"
    return 1
  fi
}

# Check required tools
check_requirements() {
  local missing_tools=()

  for tool in docker jq git python3; do
    if ! command_exists "$tool"; then
      missing_tools+=("$tool")
    fi
  done

  if [[ ${#missing_tools[@]} -gt 0 ]]; then
    log_error "Missing required tools: ${missing_tools[*]}"
    return 1
  fi

  return 0
}
