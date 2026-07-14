// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <node/profile_args.h>

#include <common/args.h>
#include <common/system.h>
#include <node/caches.h>
#include <tinyformat.h>
#include <util/byte_units.h>
#include <util/log.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace node {
namespace {

//! Highest -dbcache a RAM-scaled profile will request (MiB).
constexpr int64_t MAX_SCALED_DBCACHE_MIB{4096};
//! Cap for 32-bit builds, which cannot address a large cache.
constexpr int64_t MAX_SCALED_DBCACHE_MIB_32BIT{1024};

struct ProfileDef {
    std::string_view name;
    ProfileOptions options;
    //! Fill dbcache from detected RAM (see CalculateHighResourceDbCacheMiB).
    bool scale_to_ram{false};
};

// Each profile roughly targets a category of machine we see most frequently.
//
// - pruned-tiny:  single-board computer (~1-2 GiB RAM, small flash). Small prune,
//                 minimal caches, a reduced validation cache, and par=1 so
//                 validation leaves CPU for the host.
// - pruned-low:   small VPS or ageing laptop (~4 GiB RAM).
// - performance:  dedicated machine (>=16 GiB RAM); scale the cache to RAM up
//                 to the profile cap for fast IBD and validation. Default peer limits.
// - server:       public-facing node serving many peers; scale the cache up to
//                 the profile cap and widen network limits.
const std::array PROFILES{
    ProfileDef{"pruned-tiny", ProfileOptions{
                                  .dbcache_mib = 100,
                                  .par = 1,
                                  .maxsigcachesize_mib = 8,
                                  .prune_mib = 2048,
                                  .maxconnections = 40,
                                  .maxreceivebuffer = 2000,
                                  .maxsendbuffer = 500,
                              }},
    ProfileDef{"pruned-low", ProfileOptions{
                                 .dbcache_mib = 300,
                                 .par = 2,
                                 .maxsigcachesize_mib = 16,
                                 .prune_mib = 10240,
                                 .maxconnections = 60,
                             }},
    ProfileDef{"performance", ProfileOptions{}, /*scale_to_ram=*/true},
    ProfileDef{"server", ProfileOptions{
                             .maxconnections = 250,
                             .maxreceivebuffer = 10000,
                             .maxsendbuffer = 2000,
                         },
               /*scale_to_ram=*/true},
};

const ProfileDef* FindProfile(std::string_view name)
{
    for (const auto& def : PROFILES) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

void SoftSetProfileArg(ArgsManager& args, std::string_view profile_name, std::string_view arg, int64_t value)
{
    const auto value_str{util::ToString(value)};
    const std::string arg_str{arg};
    if (args.SoftSetArg(arg_str, value_str)) {
        LogInfo("parameter interaction: -profile=%s -> setting %s=%s\n", std::string{profile_name}, arg_str, value_str);
    }
}

void SoftSetProfileOptions(ArgsManager& args, std::string_view profile_name, const ProfileOptions& options)
{
    if (options.dbcache_mib) SoftSetProfileArg(args, profile_name, "-dbcache", *options.dbcache_mib);
    if (options.par) SoftSetProfileArg(args, profile_name, "-par", *options.par);
    if (options.maxsigcachesize_mib) SoftSetProfileArg(args, profile_name, "-maxsigcachesize", *options.maxsigcachesize_mib);
    if (options.prune_mib) SoftSetProfileArg(args, profile_name, "-prune", *options.prune_mib);
    if (options.maxconnections) SoftSetProfileArg(args, profile_name, "-maxconnections", *options.maxconnections);
    if (options.maxreceivebuffer) SoftSetProfileArg(args, profile_name, "-maxreceivebuffer", *options.maxreceivebuffer);
    if (options.maxsendbuffer) SoftSetProfileArg(args, profile_name, "-maxsendbuffer", *options.maxsendbuffer);
}

} // namespace

std::string ListProfiles()
{
    std::vector<std::string> names;
    names.reserve(PROFILES.size());
    for (const auto& def : PROFILES) {
        names.emplace_back(def.name);
    }
    return util::Join(names, ", ");
}

int64_t CalculateHighResourceDbCacheMiB(std::optional<uint64_t> total_ram_bytes, int64_t fallback_mib)
{
    if (!total_ram_bytes) return fallback_mib;

    int64_t dbcache_mib{int64_t(*total_ram_bytes / 2 / 1_MiB)};
    dbcache_mib = std::min<int64_t>(dbcache_mib, MAX_SCALED_DBCACHE_MIB);
    if constexpr (sizeof(void*) == 4) {
        dbcache_mib = std::min<int64_t>(dbcache_mib, MAX_SCALED_DBCACHE_MIB_32BIT);
    }
    return std::max<int64_t>(dbcache_mib, MIN_DB_CACHE / 1_MiB);
}

std::optional<ProfileOptions> GetProfileOptions(std::string_view name, std::optional<uint64_t> total_ram_bytes, int64_t fallback_dbcache_mib)
{
    const auto* def{FindProfile(name)};
    if (!def) return std::nullopt;

    ProfileOptions options{def->options};
    if (def->scale_to_ram) {
        const int64_t dbcache_mib{CalculateHighResourceDbCacheMiB(total_ram_bytes, fallback_dbcache_mib)};
        options.dbcache_mib = dbcache_mib;
    }
    return options;
}

std::optional<std::string> GetProfileError(const ArgsManager& args)
{
    const auto arg{args.GetArg("-profile")};
    if (!arg) return std::nullopt;

    if (FindProfile(*arg)) return std::nullopt;

    return strprintf("Unknown -profile value '%s'. Valid values are: %s.", *arg, ListProfiles());
}

void ApplyProfileArgs(ArgsManager& args)
{
    const auto arg{args.GetArg("-profile")};
    if (!arg) return;

    const std::optional<uint64_t> total_ram{GetTotalRAM()};
    const auto options{GetProfileOptions(*arg, total_ram, GetDefaultDBCache() / 1_MiB)};
    if (!options) return;

    LogInfo("parameter interaction: applying -profile=%s\n", *arg);
    SoftSetProfileOptions(args, *arg, *options);
}

} // namespace node
