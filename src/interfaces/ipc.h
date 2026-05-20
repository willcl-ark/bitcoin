// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_IPC_H
#define BITCOIN_INTERFACES_IPC_H

#include <memory>
#include <string>

namespace ipc {
namespace capnp {
class NativeConnection;
} // namespace capnp
} // namespace ipc

namespace interfaces {
class Init;

//! Interface providing access to interprocess-communication (IPC)
//! functionality. The IPC implementation is responsible for establishing
//! connections between a controlling process and a process being controlled.
//! When a connection is established, the process being controlled returns a
//! native Cap'n Proto connection to the controlling process, which the
//! controlling process can use to get access to other remote capabilities.
//!
//! When spawning a new process, the steps are:
//!
//! 1. The controlling process calls interfaces::Ipc::spawnProcess(), which
//!    calls ipc::Process::spawn(), which spawns a new process and returns a
//!    socketpair file descriptor for communicating with it.
//!    interfaces::Ipc::spawnProcess() then calls ipc::Protocol::connect()
//!    passing the socketpair descriptor, which returns a native Cap'n Proto
//!    connection.
//! 2. The spawned process calls interfaces::Ipc::startSpawnProcess(), which
//!    calls ipc::Process::checkSpawned() to read command line arguments and
//!    determine whether it is a spawned process and what socketpair file
//!    descriptor it should use. It then calls ipc::Protocol::serve() to handle
//!    incoming requests from the socketpair and invoke interfaces::Init
//!    interface methods, and exit when the socket is closed.
//! 3. The controlling process sends Cap'n Proto requests through the connection
//!    to make other remote capabilities. It can also destroy the connection to
//!    close the socket and shut down the spawned process.
//!
//! When connecting to an existing process, the steps are similar to spawning a
//! new process, except a socket is created instead of a socketpair, and
//! destroying an Init interface doesn't end the process, since there can be
//! multiple connections.
class Ipc
{
public:
    virtual ~Ipc() = default;

    //! Spawn a child process returning a native Cap'n Proto connection to it.
    virtual std::unique_ptr<ipc::capnp::NativeConnection> spawnProcess(const char* exe_name) = 0;

    //! If this is a spawned process, block and handle requests from the parent
    //! process by forwarding them to this process's Init interface, then return
    //! true. If this is not a spawned child process, return false.
    virtual bool startSpawnedProcess(int argc, char* argv[], int& exit_status) = 0;

    //! Connect to a socket address and return a native Cap'n Proto connection.
    //! Returns a non-null pointer if the connection was established, returns
    //! null if address is empty ("") or disabled ("0") or if a connection was
    //! refused but not required ("auto"), and throws an exception if there was
    //! an unexpected error.
    virtual std::unique_ptr<ipc::capnp::NativeConnection> connectAddress(std::string& address) = 0;

    //! Listen on a socket address exposing this process's init interface to
    //! clients. Throws an exception if there was an error.
    virtual void listenAddress(std::string& address) = 0;

    //! Disconnect any incoming connections that are still connected.
    virtual void disconnectIncoming() = 0;

};

//! Return implementation of Ipc interface.
std::unique_ptr<Ipc> MakeIpc(const char* exe_name, const char* process_argv0, Init& init);
} // namespace interfaces

#endif // BITCOIN_INTERFACES_IPC_H
