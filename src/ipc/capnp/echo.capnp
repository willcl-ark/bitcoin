# Copyright (c) 2021-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0x888b4f7f51e691f7;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ipc::capnp::messages");

interface Echo {
    echo @0 (echo: Text) -> (result :Text);
}
