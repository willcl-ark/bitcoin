// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CACHES_H
#define BITCOIN_KERNEL_CACHES_H

#include <util/byte_units.h>

#include <algorithm>
#include <cstdint>
#include <limits>

//! Minimum total database cache (bytes)
inline constexpr uint64_t MIN_DBCACHE_BYTES{4_MiB};
//! Maximum total database cache on current architecture (bytes)
inline constexpr uint64_t MAX_DBCACHE_BYTES{sizeof(void*) == 4 ? 1_GiB : std::numeric_limits<uint64_t>::max()};
//! Suggested default amount of cache reserved for the kernel (bytes)
inline constexpr uint64_t DEFAULT_KERNEL_CACHE{450_MiB};
//! Default LevelDB write batch size
inline constexpr uint64_t DEFAULT_DB_CACHE_BATCH{32_MiB};

//! Max memory allocated to coin DB specific cache (bytes)
inline constexpr uint64_t MAX_COINS_DB_CACHE{10_MiB};

namespace kernel {
struct CacheSizes {
    uint64_t coins_db;
    uint64_t coins;

    CacheSizes(uint64_t total_cache)
    {
        coins_db = std::min(total_cache / 2, MAX_COINS_DB_CACHE);
        total_cache -= coins_db;
        coins = total_cache; // the rest goes to the coins cache
    }
};
} // namespace kernel

#endif // BITCOIN_KERNEL_CACHES_H
