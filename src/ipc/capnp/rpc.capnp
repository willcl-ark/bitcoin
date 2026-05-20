# Copyright (c) 2025 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0x9c3505dc45e146ac;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ipc::capnp::messages");

interface Rpc {
    executeRpc @0 (request :Text, uri :Text, user :Text) -> (result :Text);
}
