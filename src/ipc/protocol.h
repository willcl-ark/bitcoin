// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_PROTOCOL_H
#define BITCOIN_IPC_PROTOCOL_H

#include <interfaces/init.h>

#include <functional>
#include <memory>

namespace ipc {
namespace capnp {
class NativeConnection;
} // namespace capnp

//! IPC protocol interface for calling IPC methods over sockets.
//!
//! There may be different implementations of this interface for different IPC
//! protocols (e.g. Cap'n Proto, gRPC, JSON-RPC, or custom protocols).
class Protocol
{
public:
    virtual ~Protocol() = default;

    //! Return native connection using the given socket descriptor. Callers send
    //! requests through the returned connection and wait on its wait scope.
    virtual std::unique_ptr<capnp::NativeConnection> connect(int fd, const char* exe_name) = 0;

    //! Listen for connections on provided socket descriptor, accept them, and
    //! handle requests on accepted connections. This method doesn't block, and
    //! performs I/O on a background thread.
    virtual void listen(int listen_fd, const char* exe_name, interfaces::Init& init) = 0;

    //! Handle requests on provided socket descriptor, forwarding them to the
    //! provided Init interface. Socket communication is handled on the
    //! current thread, and this call blocks until the socket is closed.
    //!
    //! @note: If this method is called, it needs be called before connect() or
    //! listen() methods, because for ease of implementation it's inflexible and
    //! always runs the event loop in the foreground thread. It can share its
    //! event loop with the other methods but can't share an event loop that was
    //! created by them. This isn't really a problem because serve() is only
    //! called by spawned child processes that call it immediately to
    //! communicate back with parent processes.
    //
    //! The optional `ready_fn` callback will be called after the event loop is
    //! created but before it is started. This can be useful in tests to trigger
    //! client connections from another thread as soon as the event loop is
    //! available, but should not be necessary in normal code which starts
    //! clients and servers independently.
    virtual void serve(int fd, const char* exe_name, interfaces::Init& init, const std::function<void()>& ready_fn = {}) = 0;

    //! Disconnect any incoming connections that are still connected.
    virtual void disconnectIncoming() = 0;
};
} // namespace ipc

#endif // BITCOIN_IPC_PROTOCOL_H
