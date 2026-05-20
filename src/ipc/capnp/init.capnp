# Copyright (c) 2021-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0xf2c5cfa319406aa6;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ipc::capnp::messages");

using Echo = import "echo.capnp";
using Mining = import "mining.capnp";
using Rpc = import "rpc.capnp";

interface Init {
    makeEcho @0 () -> (result :Echo.Echo);
    makeMining @1 () -> (result :Mining.Mining);
    makeRpc @2 () -> (result :Rpc.Rpc);
}
