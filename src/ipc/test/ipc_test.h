// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_TEST_IPC_TEST_H
#define BITCOIN_IPC_TEST_IPC_TEST_H

#include <util/fs.h>

void IpcNativeSocketPairTest();
void IpcSocketPairTest();
void IpcSocketTest(const fs::path& datadir);
void IpcConversionTest();
void IpcWorkerQueueTest();
void IpcEventLoopDispatcherTest();

#endif // BITCOIN_IPC_TEST_IPC_TEST_H
