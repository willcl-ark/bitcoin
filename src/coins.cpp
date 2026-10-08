// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>

#include <consensus/consensus.h>
#include <primitives/block.h>
#include <random.h>
#include <uint256.h>
#include <util/log.h>
#include <util/threadpool.h>
#include <util/trace.h>

#include <ranges>
#include <unordered_set>

TRACEPOINT_SEMAPHORE(utxocache, add);
TRACEPOINT_SEMAPHORE(utxocache, spent);
TRACEPOINT_SEMAPHORE(utxocache, uncache);

SaltedCoinsCacheHasher::SaltedCoinsCacheHasher(bool deterministic)
    : m_hasher{
          deterministic ? 0x8e819f2607a18de6 : FastRandomContext().rand64(),
          deterministic ? 0xf4020d2e3983b0eb : FastRandomContext().rand64()}
{
}

CoinsViewEmpty& CoinsViewEmpty::Get()
{
    static CoinsViewEmpty instance;
    return instance;
}

std::optional<Coin> CCoinsViewCache::PeekCoin(const COutPoint& outpoint) const
{
    auto& coins{GetStorage(outpoint).coins};
    if (auto it{coins.find(outpoint)}; it != coins.end() && !it->second.IsConfirmedUnknown()) {
        return it->second.coin.IsSpent() ? std::nullopt : std::optional{it->second.coin};
    }
    return base->PeekCoin(outpoint);
}

CCoinsViewCache::CCoinsViewCache(CCoinsView* in_base, bool deterministic)
    : CCoinsViewBacked(in_base), m_deterministic{deterministic}, m_partition_hasher{deterministic}, m_storage{[deterministic]<size_t... I>(std::index_sequence<I...>) {
          return std::array<CoinsCacheStorage, COINS_CACHE_PARTITIONS>{(static_cast<void>(I), CoinsCacheStorage{deterministic})...};
      }(std::make_index_sequence<COINS_CACHE_PARTITIONS>{})}
{
}

size_t CCoinsViewCache::DynamicMemoryUsage() const
{
    size_t usage{0};
    for (const auto& storage : m_storage) {
        usage += memusage::DynamicUsage(storage.coins) + storage.coins_usage;
    }
    return usage;
}

std::optional<Coin> CCoinsViewCache::FetchCoinFromBase(const COutPoint& outpoint) const
{
    return base->GetCoin(outpoint);
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(CoinsCacheStorage& storage, const COutPoint& outpoint) const
{
    const auto [ret, inserted] = storage.coins.try_emplace(outpoint);
    if (inserted || ret->second.IsConfirmedUnknown()) {
        if (auto coin{FetchCoinFromBase(outpoint)}) {
            ret->second.coin = std::move(*coin);
            storage.coins_usage += ret->second.coin.DynamicMemoryUsage();
            Assert(!ret->second.coin.IsSpent());
        } else if (inserted) {
            storage.coins.erase(ret);
            return storage.coins.end();
        }
        ret->second.SetConfirmedUnknown(false);
    }
    return ret;
}

std::optional<Coin> CCoinsViewCache::GetCoin(const COutPoint& outpoint) const
{
    auto& storage{GetStorage(outpoint)};
    if (auto it{FetchCoin(storage, outpoint)}; it != storage.coins.end() && !it->second.coin.IsSpent()) return it->second.coin;
    return std::nullopt;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin&& coin, bool possible_overwrite) {
    Assert(!m_active_batch);
    auto& storage{GetStorage(outpoint)};
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = storage.coins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // If the coin exists in this cache as a spent coin and is DIRTY, then
        // its spentness hasn't been flushed to the parent cache. We're
        // re-adding the coin to this cache now but we can't mark it as FRESH.
        // If we mark it FRESH and then spend it before the cache is flushed
        // we would remove it from this cache and would never flush spentness
        // to the parent cache.
        //
        // Re-adding a spent coin can happen in the case of a re-org (the coin
        // is 'spent' when the block adding it is disconnected and then
        // re-added when it is also added in a newly connected block).
        //
        // If the coin doesn't exist in the current cache, or is spent but not
        // DIRTY, then it can be marked FRESH.
        fresh = !it->second.IsDirty();
    }
    if (!inserted) {
        Assume(TrySub(storage.dirty_count, it->second.IsDirty()));
        Assume(TrySub(storage.coins_usage, it->second.coin.DynamicMemoryUsage()));
    }
    it->second.coin = std::move(coin);
    CCoinsCacheEntry::SetDirty(*it, storage.sentinel);
    ++storage.dirty_count;
    if (fresh) CCoinsCacheEntry::SetFresh(*it, storage.sentinel);
    storage.coins_usage += it->second.coin.DynamicMemoryUsage();
    TRACEPOINT(utxocache, add,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
}

void CCoinsViewCache::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) {
    Assert(!m_active_batch);
    auto& storage{GetStorage(outpoint)};
    const auto mem_usage{coin.DynamicMemoryUsage()};
    auto [it, inserted] = storage.coins.try_emplace(outpoint, std::move(coin));
    if (inserted) {
        CCoinsCacheEntry::SetDirty(*it, storage.sentinel);
        ++storage.dirty_count;
        storage.coins_usage += mem_usage;
    }
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight, bool check_for_overwrite) {
    bool fCoinbase = tx.IsCoinBase();
    const Txid& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        bool overwrite = check_for_overwrite ? cache.HaveCoin(COutPoint(txid, i)) : fCoinbase;
        // Coinbase transactions can always be overwritten, in order to correctly
        // deal with the pre-BIP30 occurrences of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), Coin(tx.vout[i], nHeight, fCoinbase), overwrite);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin* moveout) {
    Assert(!m_active_batch);
    auto& storage{GetStorage(outpoint)};
    auto it{FetchCoin(storage, outpoint)};
    if (it == storage.coins.end()) return false;
    Assume(TrySub(storage.dirty_count, it->second.IsDirty()));
    Assume(TrySub(storage.coins_usage, it->second.coin.DynamicMemoryUsage()));
    TRACEPOINT(utxocache, spent,
               outpoint.hash.data(),
               (uint32_t)outpoint.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.IsFresh()) {
        storage.coins.erase(it);
    } else {
        CCoinsCacheEntry::SetDirty(*it, storage.sentinel);
        ++storage.dirty_count;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint& outpoint) const
{
    auto& storage{GetStorage(outpoint)};
    if (auto it{FetchCoin(storage, outpoint)}; it != storage.coins.end()) return it->second.coin;
    return coinEmpty;
}

bool CCoinsViewCache::HaveCoin(const COutPoint& outpoint) const
{
    auto& storage{GetStorage(outpoint)};
    const auto it{FetchCoin(storage, outpoint)};
    return it != storage.coins.end() && !it->second.coin.IsSpent();
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint& outpoint) const
{
    auto& coins{GetStorage(outpoint).coins};
    const auto it{coins.find(outpoint)};
    return it != coins.end() && !it->second.coin.IsSpent();
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (m_block_hash.IsNull())
        m_block_hash = base->GetBestBlock();
    return m_block_hash;
}

void CCoinsViewCache::SetBestBlock(const uint256& in_block_hash)
{
    Assert(!m_active_batch);
    m_block_hash = in_block_hash;
}

void CCoinsViewCache::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& in_block_hash)
{
    Assert(!m_active_batch);
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) { // TODO a cursor can only contain dirty entries
            continue;
        }
        auto& storage{GetStorage(it->first)};
        auto [itUs, inserted]{storage.coins.try_emplace(it->first)};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                storage.coins.erase(itUs); // TODO fresh coins should have been removed at spend
            } else {
                // The parent cache does not have an entry, while the child cache does.
                // Move the data up and mark it as dirty.
                CCoinsCacheEntry& entry{itUs->second};
                assert(entry.coin.DynamicMemoryUsage() == 0);
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
                CCoinsCacheEntry::SetDirty(*itUs, storage.sentinel);
                ++storage.dirty_count;
                storage.coins_usage += entry.coin.DynamicMemoryUsage();
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.IsFresh()) CCoinsCacheEntry::SetFresh(*itUs, storage.sentinel);
            }
        } else {
            // Found the entry in the parent cache
            if (it->second.IsFresh() && !itUs->second.coin.IsSpent()) {
                // The coin was marked FRESH in the child cache, but the coin
                // exists in the parent cache. If this ever happens, it means
                // the FRESH flag was misapplied and there is a logic error in
                // the calling code.
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }

            if (itUs->second.IsFresh() && it->second.coin.IsSpent()) {
                // The grandparent cache does not have an entry, and the coin
                // has been spent. We can just delete it from the parent cache.
                Assume(TrySub(storage.dirty_count, itUs->second.IsDirty()));
                Assume(TrySub(storage.coins_usage, itUs->second.coin.DynamicMemoryUsage()));
                storage.coins.erase(itUs);
            } else {
                // A normal modification.
                Assume(TrySub(storage.coins_usage, itUs->second.coin.DynamicMemoryUsage()));
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    itUs->second.coin = std::move(it->second.coin);
                } else {
                    itUs->second.coin = it->second.coin;
                }
                storage.coins_usage += itUs->second.coin.DynamicMemoryUsage();
                if (!itUs->second.IsDirty()) {
                    CCoinsCacheEntry::SetDirty(*itUs, storage.sentinel);
                    ++storage.dirty_count;
                }
                // NOTE: It isn't safe to mark the coin as FRESH in the parent
                // cache. If it already existed and was spent in the parent
                // cache then marking it FRESH would prevent that spentness
                // from being flushed to the grandparent.
            }
        }
    }
    SetBestBlock(in_block_hash);
}

void CCoinsViewCache::Flush(bool reallocate_cache)
{
    Assert(!m_active_batch);
    auto cursor{CoinsViewCacheCursor(m_storage, /*will_erase=*/true)};
    base->BatchWrite(cursor, m_block_hash);
    for (auto& storage : m_storage) {
        Assume(storage.dirty_count == 0);
        storage.coins.clear();
        storage.coins_usage = 0;
    }
    if (reallocate_cache) ReallocateCache();
}

void CCoinsViewCache::Sync()
{
    Assert(!m_active_batch);
    auto cursor{CoinsViewCacheCursor(m_storage, /*will_erase=*/false)};
    base->BatchWrite(cursor, m_block_hash);
    for (const auto& storage : m_storage) {
        Assume(storage.dirty_count == 0);
        if (storage.sentinel.second.Next() != &storage.sentinel) {
            /* BatchWrite must clear flags of all entries */
            throw std::logic_error("Not all unspent flagged entries were cleared");
        }
    }
}

void CCoinsViewCache::Reset() noexcept
{
    Assert(!m_active_batch);
    for (auto& storage : m_storage) {
        storage.coins.clear();
        storage.coins_usage = 0;
        storage.dirty_count = 0;
    }
    SetBestBlock(uint256::ZERO);
}

void CCoinsViewCache::Uncache(const COutPoint& hash)
{
    Assert(!m_active_batch);
    auto& storage{GetStorage(hash)};
    CCoinsMap::iterator it = storage.coins.find(hash);
    if (it != storage.coins.end() && !it->second.IsDirty()) {
        Assume(TrySub(storage.coins_usage, it->second.coin.DynamicMemoryUsage()));
        TRACEPOINT(utxocache, uncache,
               hash.hash.data(),
               (uint32_t)hash.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
        storage.coins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const
{
    size_t count{0};
    for (const auto& storage : m_storage) {
        count += storage.coins.size();
    }
    return count;
}

size_t CCoinsViewCache::GetDirtyCount() const noexcept
{
    size_t count{0};
    for (const auto& storage : m_storage) {
        count += storage.dirty_count;
    }
    return count;
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!HaveCoin(tx.vin[i].prevout)) {
                return false;
            }
        }
    }
    return true;
}

std::vector<const Coin*> CCoinsViewCache::ResolveInputs(const CTransaction& tx) const
{
    std::vector<const Coin*> inputs;
    if (tx.IsCoinBase()) return inputs;

    inputs.reserve(tx.vin.size());
    for (const auto& txin : tx.vin) {
        const Coin& coin{AccessCoin(txin.prevout)};
        inputs.push_back(coin.IsSpent() ? nullptr : &coin);
    }
    return inputs;
}

void CCoinsViewCache::ReallocateCache()
{
    Assert(!m_active_batch);
    for (auto& storage : m_storage) {
        assert(storage.coins.empty());
        storage.~CoinsCacheStorage();
        ::new (&storage) CoinsCacheStorage{m_deterministic};
    }
}

void CCoinsViewCache::SanityCheck() const
{
    for (const auto& storage : m_storage) {
        size_t recomputed_usage = 0;
        size_t count_dirty = 0;
        for (const auto& [_, entry] : storage.coins) {
            if (entry.coin.IsSpent()) {
                assert(entry.IsDirty() && !entry.IsFresh()); // A spent coin must be dirty and cannot be fresh
            } else {
                assert(entry.IsDirty() || !entry.IsFresh()); // An unspent coin must not be fresh if not dirty
            }

            // Recompute storage.coins_usage.
            recomputed_usage += entry.coin.DynamicMemoryUsage();

            // Count the number of entries we expect in the linked list.
            if (entry.IsDirty()) ++count_dirty;
        }
        // Iterate over the linked list of flagged entries.
        size_t count_linked = 0;
        for (auto it = storage.sentinel.second.Next(); it != &storage.sentinel; it = it->second.Next()) {
            // Verify linked list integrity.
            assert(it->second.Next()->second.Prev() == it);
            assert(it->second.Prev()->second.Next() == it);
            // Verify they are actually flagged.
            assert(it->second.IsDirty());
            // Count the number of entries actually in the list.
            ++count_linked;
        }
        assert(count_dirty == count_linked && count_dirty == storage.dirty_count);
        assert(recomputed_usage == storage.coins_usage);
    }
}

CCoinsViewCache::ResetGuard CoinsViewOverlay::StartFetching(const CBlock& block LIFETIMEBOUND) noexcept
{
    Assert(m_futures.empty());
    Assert(m_inputs.empty());
    Assert(m_input_head.load(std::memory_order_relaxed) == 0);
    Assert(m_input_tail == 0);
    if (const auto workers_count{m_thread_pool->WorkersCount()}; workers_count > 0 && block.vtx.size() > 1) {
        // Loop through the block inputs and set their prevouts in the queue.
        // Filter inputs that spend outputs created earlier in the same block. These outputs will be created
        // directly in the cache from the tx that creates them, so they will not be requested from a base view.
        std::unordered_set<Txid, SaltedCoinsCacheHasher> earlier_txids;
        earlier_txids.reserve(block.vtx.size());
        earlier_txids.emplace(block.vtx[0]->GetHash());
        for (const auto& tx : block.vtx | std::views::drop(1)) {
            for (const auto& input : tx->vin) {
                if (!earlier_txids.contains(input.prevout.hash)) m_inputs.emplace_back(input.prevout);
            }
            earlier_txids.emplace(tx->GetHash());
        }
        // Only submit tasks if we have something to fetch.
        if (m_inputs.size()) {
            std::vector<std::function<void()>> tasks(workers_count, [this] {
                while (ProcessInput()) {}
            });
            if (auto futures{m_thread_pool->Submit(std::move(tasks))}) {
                m_futures = std::move(*futures);
            } else {
                // Submit can fail if a shared owner of the thread pool outside of this class calls Stop() or
                // Interrupt() on a different thread after we call WorkersCount() above. In that case parallel
                // fetching will not make progress, so we clear the inputs to fall back to single threaded fetching.
                LogWarning("Failed to submit prevout fetch tasks (%s); falling back to single-threaded fetching for this block.", SubmitErrorString(futures.error()));
                m_inputs.clear();
                StopFetching(); // Assert nothing changed if we failed to start tasks.
            }
        }
    }
    return CreateResetGuard();
}

static const uint64_t MIN_TRANSACTION_OUTPUT_WEIGHT{WITNESS_SCALE_FACTOR * ::GetSerializeSize(CTxOut())};
static const uint64_t MAX_OUTPUTS_PER_BLOCK{MAX_BLOCK_WEIGHT / MIN_TRANSACTION_OUTPUT_WEIGHT};

const Coin& AccessByTxid(const CCoinsViewCache& view, const Txid& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const Coin& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}

template <typename ReturnType, typename Func>
static ReturnType ExecuteBackedWrapper(Func func, const std::vector<std::function<void()>>& err_callbacks)
{
    try {
        return func();
    } catch(const std::runtime_error& e) {
        for (const auto& f : err_callbacks) {
            f();
        }
        LogError("Error reading from database: %s\n", e.what());
        // Starting the shutdown sequence and returning false to the caller would be
        // interpreted as 'entry not found' (as opposed to unable to read data), and
        // could lead to invalid interpretation. Just exit immediately, as we can't
        // continue anyway, and all writes should be atomic.
        std::abort();
    }
}

std::optional<Coin> CCoinsViewErrorCatcher::GetCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::GetCoin(outpoint); }, m_err_callbacks);
}

bool CCoinsViewErrorCatcher::HaveCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<bool>([&]() { return CCoinsViewBacked::HaveCoin(outpoint); }, m_err_callbacks);
}

std::optional<Coin> CCoinsViewErrorCatcher::PeekCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::PeekCoin(outpoint); }, m_err_callbacks);
}
