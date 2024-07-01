// Copyright (c) 2024- The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MACROS_H
#define BITCOIN_CRYPTO_MACROS_H

#include <util/macros.h>

#define SHA256_INTRINSIC_TARGET_CLANG(x) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wattributes\"") \
    _Pragma(STRINGIZE(clang attribute push(__attribute__((target(x))), apply_to = function)))

#define SHA256_INTRINSIC_TARGET_GCC(x) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wattributes\"") \
    _Pragma(STRINGIZE(GCC target (x)))

#define SHA256_INTRINSIC_TARGET_END_CLANG \
    _Pragma("clang attribute pop") \
    _Pragma("GCC diagnostic pop")

#define SHA256_INTRINSIC_TARGET_END_GCC \
    _Pragma("GCC diagnostic pop")

#define SHA256_INTRINSIC_TARGET_MSVC(x) \
    __pragma(push_macro("__target__")) \
    __pragma(optimize("gt", on))

#define SHA256_INTRINSIC_TARGET_END_MSVC \
    __pragma(pop_macro("__target__"))

#if defined(_MSC_VER)
    #define SHA256_INTRINSIC_TARGET(x) SHA256_INTRINSIC_TARGET_MSVC(x)
    #define SHA256_INTRINSIC_TARGET_END SHA256_INTRINSIC_TARGET_END_MSVC
#elif defined(__clang__)
    #define SHA256_INTRINSIC_TARGET(x) SHA256_INTRINSIC_TARGET_CLANG(x)
    #define SHA256_INTRINSIC_TARGET_END SHA256_INTRINSIC_TARGET_END_CLANG
#elif defined(__GNUC__)
    #define SHA256_INTRINSIC_TARGET(x) SHA256_INTRINSIC_TARGET_GCC(x)
    #define SHA256_INTRINSIC_TARGET_END SHA256_INTRINSIC_TARGET_END_GCC
#else
    #define SHA256_INTRINSIC_TARGET(x)
    #define SHA256_INTRINSIC_TARGET_END
#endif

#endif // BITCOIN_CRYPTO_MACROS_H
