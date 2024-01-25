// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/checks.h>

#include <key.h>
#include <random.h>
#include <util/time.h>

#include <memory>

namespace kernel {

util::Result<void> SanityChecks(const Context&)
{
    if (!ECC_InitSanityCheck()) {
        return util::Error{"Elliptic curve cryptography sanity check failure. Aborting."};
    }

    if (!Random_SanityCheck()) {
        return util::Error{"OS cryptographic RNG sanity check failure. Aborting."};
    }

    return {};
}

}
