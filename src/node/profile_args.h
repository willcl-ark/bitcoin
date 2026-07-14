// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#ifndef BITCOIN_NODE_PROFILE_ARGS_H
#define BITCOIN_NODE_PROFILE_ARGS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class ArgsManager;

namespace node {

/** Startup options overridden by a resource profile. Empty fields are left unchanged. */
struct ProfileOptions {
    /** `-dbcache` value in MiB. */
    std::optional<int64_t> dbcache_mib{};
    /** `-par` script verification thread count. */
    std::optional<int64_t> par{};
    /** `-maxsigcachesize` value in MiB. */
    std::optional<int64_t> maxsigcachesize_mib{};
    /** `-prune` target in MiB. */
    std::optional<int64_t> prune_mib{};
    /** `-maxconnections` peer count. */
    std::optional<int64_t> maxconnections{};
    /** `-maxreceivebuffer` value in thousands of bytes. */
    std::optional<int64_t> maxreceivebuffer{};
    /** `-maxsendbuffer` value in thousands of bytes. */
    std::optional<int64_t> maxsendbuffer{};
};

/** Return the available profile names as a comma-separated string. */
std::string ListProfiles();

/**
 * Calculate the `-dbcache` value for a RAM-scaled profile.
 *
 * @param[in] total_ram_bytes Detected total system RAM, if available.
 * @param[in] fallback_mib Value to use when RAM detection is unavailable.
 * @return The calculated cache size in MiB.
 */
int64_t CalculateHighResourceDbCacheMiB(std::optional<uint64_t> total_ram_bytes, int64_t fallback_mib);

/**
 * Return options for a profile, or `std::nullopt` for an unknown profile.
 *
 * @param[in] name Profile name.
 * @param[in] total_ram_bytes Detected total system RAM, if available.
 * @param[in] fallback_dbcache_mib Value to use when RAM detection is unavailable.
 */
std::optional<ProfileOptions> GetProfileOptions(std::string_view name, std::optional<uint64_t> total_ram_bytes, int64_t fallback_dbcache_mib);

/** Return an error for an unknown `-profile` argument, if one is set. */
std::optional<std::string> GetProfileError(const ArgsManager& args);

/** Apply the selected profile using soft-set argument precedence. */
void ApplyProfileArgs(ArgsManager& args);

} // namespace node

#endif // BITCOIN_NODE_PROFILE_ARGS_H
