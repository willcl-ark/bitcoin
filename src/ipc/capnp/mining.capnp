# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0xc77d03df6a41b505;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ipc::capnp::messages");

using Common = import "common.capnp";

const maxMoney :Int64 = 2100000000000000;
const maxDouble :Float64 = 1.7976931348623157e308;
const defaultBlockReservedWeight :UInt32 = 8000;
const defaultCoinbaseOutputMaxAdditionalSigops :UInt32 = 400;

interface Mining {
    isTestChain @0 () -> (result: Bool);
    isInitialBlockDownload @1 () -> (result: Bool);
    getTip @2 () -> (result: OptionalBlockRef);
    waitTipChanged @3 (currentTip: Data, timeout: Float64 = .maxDouble) -> (result: OptionalBlockRef);
    createNewBlock @4 (options: BlockCreateOptions, cooldown: Bool = true) -> (result: OptionalBlockTemplate);
    checkBlock @5 (block: Data, options: BlockCheckOptions) -> (reason: Text, debug: Text, result: Bool);
    interrupt @6 () -> ();
}

interface BlockTemplate {
    getBlockHeader @0 () -> (result: Data);
    getBlock @1 () -> (result: Data);
    getTxFees @2 () -> (result: List(Int64));
    getTxSigops @3 () -> (result: List(Int64));
    getCoinbaseTx @4 () -> (result: CoinbaseTx);
    getCoinbaseMerklePath @5 () -> (result: List(Data));
    submitSolution @6 (version: UInt32, timestamp: UInt32, nonce: UInt32, coinbase :Data) -> (result: Bool);
    waitNext @7 (options: BlockWaitOptions) -> (result: OptionalBlockTemplate);
    interruptWait @8 () -> ();
}

struct OptionalBlockRef {
    union {
        none @0 :Void;
        value @1 :Common.BlockRef;
    }
}

struct OptionalBlockTemplate {
    union {
        none @0 :Void;
        value @1 :BlockTemplate;
    }
}

struct BlockCreateOptions {
    useMempool @0 :Bool = true;
    blockReservedWeight @1 :UInt64 = .defaultBlockReservedWeight;
    coinbaseOutputMaxAdditionalSigops @2 :UInt64 = .defaultCoinbaseOutputMaxAdditionalSigops;
}

struct BlockWaitOptions {
    timeout @0 :Float64 = .maxDouble;
    feeThreshold @1 :Int64 = .maxMoney;
}

struct BlockCheckOptions {
    checkMerkleRoot @0 :Bool = true;
    checkPow @1 :Bool = true;
}

struct CoinbaseTx {
    version @0 :UInt32;
    sequence @1 :UInt32;
    scriptSigPrefix @2 :Data;
    witness @3 :Data;
    blockRewardRemaining @4 :Int64;
    requiredOutputs @5 :List(Data);
    lockTime @6 :UInt32;
}
