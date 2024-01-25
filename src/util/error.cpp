// Copyright (c) 2010-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/error.h>

#include <tinyformat.h>

#include <cassert>
#include <string>

std::string TransactionErrorString(const TransactionError err)
{
    switch (err) {
        case TransactionError::OK:
            return "No error";
        case TransactionError::MISSING_INPUTS:
            return "Inputs missing or spent";
        case TransactionError::ALREADY_IN_CHAIN:
            return "Transaction already in block chain";
        case TransactionError::P2P_DISABLED:
            return "Peer-to-peer functionality missing or disabled";
        case TransactionError::MEMPOOL_REJECTED:
            return "Transaction rejected by mempool";
        case TransactionError::MEMPOOL_ERROR:
            return "Mempool internal error";
        case TransactionError::INVALID_PSBT:
            return "PSBT is not well-formed";
        case TransactionError::PSBT_MISMATCH:
            return "PSBTs not compatible (different transactions)";
        case TransactionError::SIGHASH_MISMATCH:
            return "Specified sighash value does not match value stored in PSBT";
        case TransactionError::MAX_FEE_EXCEEDED:
            return "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)";
        case TransactionError::MAX_BURN_EXCEEDED:
            return "Unspendable output exceeds maximum configured by user (maxburnamount)";
        case TransactionError::EXTERNAL_SIGNER_NOT_FOUND:
            return "External signer not found";
        case TransactionError::EXTERNAL_SIGNER_FAILED:
            return "External signer failed to sign";
        case TransactionError::INVALID_PACKAGE:
            return "Transaction rejected due to invalid package";
        // no default case, so the compiler can warn about missing cases
    }
    assert(false);
}

std::string ResolveErrMsg(const std::string& optname, const std::string& strBind)
{
    return strprintf("Cannot resolve -%s address: '%s'", optname, strBind);
}

std::string InvalidPortErrMsg(const std::string& optname, const std::string& invalid_value)
{
    return strprintf("Invalid port specified in %s: '%s'", optname, invalid_value);
}

std::string AmountHighWarn(const std::string& optname)
{
    return strprintf("%s is set very high!", optname);
}

std::string AmountErrMsg(const std::string& optname, const std::string& strValue)
{
    return strprintf("Invalid amount for -%s=<amount>: '%s'", optname, strValue);
}
