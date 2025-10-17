// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <cuckoocache.h>
#include <random.h>
#include <script/sigcache.h>
#include <uint256.h>
#include <validation.h>

#include <vector>

/**
 * Benchmark cuckoo cache performance.
 *
 * Tests worst-case scenario: "a cache with a block that has all previously
 * unseen signatures/transactions" to maximize cache misses and evictions.
 */

static void BenchmarkCacheFanout(benchmark::Bench& bench)
{
    // Use signature cache size for consistent comparison
    const size_t cache_bytes = 8 << 20;   // 8 MiB
    const size_t num_operations = 100000; // Enough to cause evictions

    // Generate unique hashes for worst-case (all misses initially)
    std::vector<uint256> unique_hashes;
    unique_hashes.reserve(num_operations);

    FastRandomContext rng{};
    for (size_t i = 0; i < num_operations; ++i) {
        unique_hashes.push_back(rng.rand256());
    }

    bench.epochIterations(1000).epochs(10).run([&] {
        // Fresh cache for each run to ensure cold start
        SignatureCache sig_cache{cache_bytes};

        // Insert unique elements
        for (const auto& hash : unique_hashes) {
            sig_cache.Set(hash);
        }

        // Lookup all elements (test retrieval perf)
        size_t hits = 0;
        for (const auto& hash : unique_hashes) {
            if (sig_cache.Get(hash, false)) {
                hits++;
            }
        }
        // Use to prevent optimization
        (void)hits;
    });
}

/**
 * Benchmark realistic block validation scenario with current cache configuration.
 */
static void BenchmarkBlockValidation(benchmark::Bench& bench)
{
    const size_t signature_cache_bytes = 16 << 20; // 16 MiB
    const size_t script_cache_bytes = 16 << 20;    // 16 MiB

    const size_t mempool_txs = 50000; // Simulate populated mempool
    const size_t block_txs = 4000;    // Typical block size
    const double hit_rate = 0.9;      // 90% cache hit rate

    FastRandomContext rng{};

    // Pre-generate mempool transaction hashes
    std::vector<uint256> mempool_hashes;
    mempool_hashes.reserve(mempool_txs);
    for (size_t i = 0; i < mempool_txs; ++i) {
        mempool_hashes.push_back(rng.rand256());
    }

    // Generate block hashes (mix of cached and new)
    std::vector<uint256> block_hashes;
    block_hashes.reserve(block_txs);

    size_t cached_count = static_cast<size_t>(block_txs * hit_rate);
    for (size_t i = 0; i < cached_count; ++i) {
        // Use random mempool transaction
        size_t idx = rng.randrange(mempool_hashes.size());
        block_hashes.push_back(mempool_hashes[idx]);
    }

    // Add new transactions (whcih are cache misses)
    for (size_t i = cached_count; i < block_txs; ++i) {
        block_hashes.push_back(rng.rand256());
    }

    bench.epochIterations(100).epochs(10).run([&] {
        SignatureCache sig_cache{signature_cache_bytes};
        ValidationCache validation_cache{script_cache_bytes, signature_cache_bytes};

        // Populate with transactions
        for (const auto& hash : mempool_hashes) {
            sig_cache.Set(hash);
            validation_cache.m_script_execution_cache.insert(hash);
        }

        // Process block
        size_t sig_hits = 0, script_hits = 0;
        for (const auto& hash : block_hashes) {
            if (sig_cache.Get(hash, false)) sig_hits++;
            if (validation_cache.m_script_execution_cache.contains(hash, false)) script_hits++;
        }
        // Use to prevent optimization
        (void)sig_hits;
        (void)script_hits;
    });
}

BENCHMARK(BenchmarkCacheFanout, benchmark::PriorityLevel::HIGH);
BENCHMARK(BenchmarkBlockValidation, benchmark::PriorityLevel::HIGH);
