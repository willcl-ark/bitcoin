// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_CAPNP_PROTOCOL_H
#define BITCOIN_IPC_CAPNP_PROTOCOL_H

#include <ipc/capnp/init.capnp.h>

#include <functional>
#include <memory>

namespace interfaces {
class Init;
} // namespace interfaces
namespace kj {
class WaitScope;
} // namespace kj
namespace ipc {
class Protocol;
namespace capnp {
class NativeConnection
{
public:
    explicit NativeConnection(int fd);
    ~NativeConnection();
    NativeConnection(const NativeConnection&) = delete;
    NativeConnection& operator=(const NativeConnection&) = delete;

    messages::Init::Client init();
    kj::WaitScope& waitScope();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<NativeConnection> ConnectNative(int fd);
void ServeNative(int fd, interfaces::Init& init, const std::function<void()>& ready_fn = {});
std::unique_ptr<Protocol> MakeCapnpProtocol();
} // namespace capnp
} // namespace ipc

#endif // BITCOIN_IPC_CAPNP_PROTOCOL_H
