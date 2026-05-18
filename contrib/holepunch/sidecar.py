#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""TCP hole-punching raw proxy sidecar for Bitcoin Core."""

import argparse
import asyncio
from dataclasses import dataclass
from datetime import datetime
import json
import random
import re
import shlex
import socket
import time


CTRL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f]")
CONNECT_TIMEOUT = 5.0
COORDINATOR_TIMEOUT = 120.0
PUNCH_TIMEOUT = 20.0
PUNCH_RETRY = 0.25
LOCAL_CORE_TIMEOUT = 30.0
CLI_TIMEOUT = 10.0
RECONNECT_DELAY = 5.0


def sanitize(value):
    return CTRL_RE.sub("", str(value))


def log(tag, msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")
    print(f"{ts} {tag} {sanitize(msg)}", flush=True)


def parse_addr(value, default_port=None):
    if value.startswith("["):
        host, port = value[1:].split("]:", 1)
        return host, int(port)
    if value.count(":") == 1:
        host, port = value.rsplit(":", 1)
        return host, int(port)
    if default_port is None:
        raise ValueError(f"missing port in address {value!r}")
    return value, default_port


def fmt_addr(addr):
    ip, port = addr
    if ":" in ip:
        return f"[{ip}]:{port}"
    return f"{ip}:{port}"


def set_reuse(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass


async def send_msg(writer, obj):
    writer.write(json.dumps(obj).encode("utf8") + b"\n")
    await writer.drain()


async def read_msg(reader, timeout):
    line = await asyncio.wait_for(reader.readline(), timeout=timeout)
    if not line:
        raise ConnectionError("connection closed")
    return json.loads(line)


def create_sidecar_socket(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    set_reuse(sock)
    sock.bind((host, port))
    sock.listen(128)
    sock.setblocking(False)
    return sock


@dataclass
class Match:
    match_id: str
    role: str
    peer: tuple[str, int]


@dataclass
class LocalCoreConnection:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter


@dataclass
class MatchState:
    match: Match
    port: int
    claimed: bool = False


class Sidecar:
    def __init__(self, args):
        self.args = args
        self.coordinator = parse_addr(args.coordinator)
        self.bitcoind = parse_addr(args.bitcoind_host, default_port=8333)
        self.matches = {}

    def session_done(self, task, state):
        self.matches.pop(state.match.match_id, None)
        if task.cancelled():
            return
        try:
            exc = task.exception()
        except asyncio.CancelledError:
            return
        if exc is not None:
            log("SIDECAR", f"proxy session task failed: {exc!r}")

    def punch_done(self, task, state):
        if not state.claimed:
            self.matches.pop(state.match.match_id, None)
        if task.cancelled():
            return
        try:
            exc = task.exception()
        except asyncio.CancelledError:
            return
        if exc is not None:
            log("SIDECAR", f"punch task failed: {exc!r}")

    async def run(self):
        log("SIDECAR", f"bitcoind={fmt_addr(self.bitcoind)}")
        await self.registration_loop()

    async def registration_loop(self):
        while True:
            while len(self.matches) >= self.args.target_peers:
                await asyncio.sleep(5)
            try:
                await self.register_once()
            except (
                OSError,
                ConnectionError,
                asyncio.TimeoutError,
                json.JSONDecodeError,
            ) as e:
                log("COORD", f"registration failed: {e}")
                await asyncio.sleep(RECONNECT_DELAY)

    def create_rendezvous_socket(self):
        if self.args.listen_port != 0:
            return create_sidecar_socket(self.args.bind_host, self.args.listen_port)

        while True:
            sock = create_sidecar_socket(self.args.bind_host, 0)
            if sock.getsockname()[1] not in {s.port for s in self.matches.values()}:
                return sock
            sock.close()

    async def register_once(self):
        listener_sock = self.create_rendezvous_socket()
        listen_port = listener_sock.getsockname()[1]
        server = await asyncio.start_server(
            self.handle_remote_sidecar,
            sock=listener_sock,
        )
        log("LISTEN", f"{self.args.bind_host}:{listen_port}")
        server_owned = True
        writer = None
        tag = f"COORD[{fmt_addr(self.coordinator)}]"
        try:
            sock = await self.connect_from_port(self.coordinator, listen_port)
            reader, writer = await asyncio.open_connection(sock=sock)
            log(tag, "connected")
            while True:
                msg = await read_msg(reader, timeout=COORDINATOR_TIMEOUT)
                msg_type = msg.get("type")
                if msg_type == "welcome":
                    log(tag, f"observed as {fmt_addr(tuple(msg['you']))}")
                    await send_msg(
                        writer,
                        {
                            "type": "join",
                            "network": self.args.network,
                            "exclude": sorted(
                                {s.match.peer[0] for s in self.matches.values()}
                            ),
                        },
                    )
                elif msg_type == "wait":
                    log(tag, "waiting for match")
                elif msg_type == "punch":
                    match = Match(
                        match_id=msg["match"],
                        role=msg["role"],
                        peer=tuple(msg["peer"]),
                    )
                    state = MatchState(match=match, port=listen_port)
                    self.matches[match.match_id] = state
                    log(
                        f"MATCH[{match.match_id}]",
                        f"role={match.role} peer={fmt_addr(match.peer)}",
                    )
                    task = asyncio.create_task(self.punch(state, server))
                    task.add_done_callback(
                        lambda task, state=state: self.punch_done(task, state)
                    )
                    server_owned = False
                    return
        finally:
            if writer is not None:
                writer.close()
                await writer.wait_closed()
            if server_owned:
                server.close()
                await server.wait_closed()

    async def connect_from_port(self, remote, port):
        infos = await asyncio.get_running_loop().getaddrinfo(
            remote[0],
            remote[1],
            family=socket.AF_INET,
            type=socket.SOCK_STREAM,
        )
        last_error = None
        for family, socktype, proto, _, sockaddr in infos:
            sock = socket.socket(family, socktype, proto)
            set_reuse(sock)
            sock.setblocking(False)
            try:
                sock.bind((self.args.bind_host, port))
                await asyncio.wait_for(
                    asyncio.get_running_loop().sock_connect(sock, sockaddr),
                    timeout=CONNECT_TIMEOUT,
                )
                return sock
            except (OSError, asyncio.TimeoutError) as e:
                last_error = e
                sock.close()
        raise last_error or OSError("no address resolved")

    async def punch(self, state, server):
        match = state.match
        try:
            deadline = time.monotonic() + PUNCH_TIMEOUT
            while time.monotonic() < deadline and not state.claimed:
                try:
                    sock = await self.connect_from_port(match.peer, state.port)
                    reader, writer = await asyncio.open_connection(sock=sock)
                    await self.start_matched_proxy(state, reader, writer, "outbound")
                    return
                except (
                    OSError,
                    ConnectionError,
                    asyncio.TimeoutError,
                ) as e:
                    log(f"MATCH[{match.match_id}]", f"punch attempt failed: {e}")
                    await asyncio.sleep(PUNCH_RETRY + random.random() * 0.2)
            if not state.claimed:
                log(f"MATCH[{match.match_id}]", "punch timed out")
                self.matches.pop(match.match_id, None)
        finally:
            server.close()
            await server.wait_closed()

    async def handle_remote_sidecar(self, reader, writer):
        addr = writer.get_extra_info("peername")
        if addr is None:
            writer.close()
            await writer.wait_closed()
            return
        addr = (addr[0], addr[1])
        tag = f"REMOTE[{fmt_addr(addr)}]"
        port = writer.get_extra_info("sockname")[1]
        states = [state for state in self.matches.values() if state.port == port]
        if len(states) != 1:
            log(tag, "closing unclaimed connection")
            writer.close()
            await writer.wait_closed()
            return
        await self.start_matched_proxy(states[0], reader, writer, "inbound")

    async def start_matched_proxy(self, state, reader, writer, direction):
        match = state.match
        tag = f"MATCH[{match.match_id}]"
        if state.claimed:
            log(tag, f"duplicate {direction} socket; closing")
            writer.close()
            await writer.wait_closed()
            return
        state.claimed = True
        task = asyncio.create_task(self.proxy_match(match, reader, writer, direction))
        task.add_done_callback(lambda task, state=state: self.session_done(task, state))

    async def proxy_match(self, match, remote_reader, remote_writer, direction):
        tag = f"MATCH[{match.match_id}]"
        log(tag, f"{direction} socket established")
        if match.role == "receiver":
            try:
                core_reader, core_writer = await asyncio.open_connection(*self.bitcoind)
            except OSError as e:
                log(
                    tag,
                    f"could not connect to bitcoind at {fmt_addr(self.bitcoind)}: {e}",
                )
                remote_writer.close()
                await remote_writer.wait_closed()
                return
            log(
                tag, f"proxying remote sidecar to bitcoind at {fmt_addr(self.bitcoind)}"
            )
            await self.proxy_streams(
                remote_reader,
                remote_writer,
                core_reader,
                core_writer,
                tag,
            )
            return

        log(
            tag,
            "waiting for local Core outbound connection; opening temporary listener",
        )
        local = await self.open_local_core_connection(tag)
        if local is None:
            remote_writer.close()
            await remote_writer.wait_closed()
            return
        log(tag, "proxying local Core outbound connection to remote sidecar")
        await self.proxy_streams(
            local.reader,
            local.writer,
            remote_reader,
            remote_writer,
            tag,
        )

    def addnode_command(self, port):
        command = shlex.split(self.args.cli_command)
        if not command:
            raise ValueError("empty cli command")
        return command + [
            "addnode",
            f"127.0.0.1:{port}",
            "onetry",
        ]

    def addnode_command_text(self, port):
        try:
            return shlex.join(self.addnode_command(port))
        except ValueError:
            return f"{self.args.cli_command} addnode 127.0.0.1:{port} onetry"

    async def run_addnode_command(self, port, tag):
        try:
            command = self.addnode_command(port)
        except ValueError as e:
            log(tag, f"could not parse cli command: {e}")
            return False

        log(tag, f"opening local Core connection: {shlex.join(command)}")
        try:
            proc = await asyncio.create_subprocess_exec(
                *command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            stdout, stderr = await asyncio.wait_for(
                proc.communicate(),
                timeout=CLI_TIMEOUT,
            )
        except FileNotFoundError:
            log(tag, f"cli command not found; run manually: {shlex.join(command)}")
            return True
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
            log(tag, f"bitcoin-cli timed out; run manually: {shlex.join(command)}")
            return True
        except OSError as e:
            log(
                tag,
                f"could not run bitcoin-cli: {e}; run manually: {shlex.join(command)}",
            )
            return True

        if proc.returncode == 0:
            log(tag, "bitcoin-cli addnode command completed")
            return True

        output = stderr.decode("utf8", errors="replace").strip()
        if not output:
            output = stdout.decode("utf8", errors="replace").strip()
        log(
            tag,
            "bitcoin-cli addnode failed"
            f" (exit={proc.returncode}): {sanitize(output)}; "
            f"run manually: {shlex.join(command)}",
        )
        return True

    async def open_local_core_connection(self, tag):
        local_connected = asyncio.get_running_loop().create_future()

        async def handle_local_core(reader, writer):
            addr = writer.get_extra_info("peername")
            if addr is None:
                writer.close()
                await writer.wait_closed()
                return
            addr = (addr[0], addr[1])
            local_tag = f"LOCAL[{fmt_addr(addr)}]"
            if local_connected.done():
                log(local_tag, "duplicate local Core connection; closing")
                writer.close()
                await writer.wait_closed()
                return
            log(local_tag, "accepted local Core connection")
            local_connected.set_result(LocalCoreConnection(reader, writer))

        server = await asyncio.start_server(handle_local_core, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        try:
            if not await self.run_addnode_command(port, tag):
                return None
            try:
                return await asyncio.wait_for(
                    local_connected,
                    timeout=LOCAL_CORE_TIMEOUT,
                )
            except asyncio.TimeoutError:
                log(
                    tag,
                    f"timed out waiting for local Core; run: {self.addnode_command_text(port)}",
                )
                return None
        finally:
            server.close()

    async def proxy_streams(
        self,
        left_reader,
        left_writer,
        right_reader,
        right_writer,
        tag,
    ):
        async def pipe(src, dst):
            try:
                while True:
                    data = await src.read(65536)
                    if not data:
                        break
                    dst.write(data)
                    await dst.drain()
            except (ConnectionError, OSError):
                pass
            finally:
                dst.close()

        await asyncio.gather(
            pipe(left_reader, right_writer),
            pipe(right_reader, left_writer),
        )
        await asyncio.gather(
            left_writer.wait_closed(),
            right_writer.wait_closed(),
            return_exceptions=True,
        )
        log(tag, "proxy session closed")


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    hidden = argparse.SUPPRESS
    parser.add_argument(
        "--coordinator",
        default="116.202.23.118:57996",
        help=hidden,
    )
    parser.add_argument(
        "--listen-port",
        type=int,
        default=0,
        help=hidden,
    )
    parser.add_argument("--bind-host", default="0.0.0.0", help=hidden)
    parser.add_argument(
        "--bitcoind-host",
        default="127.0.0.1:8333",
        help="HOST:PORT of local bitcoind listener",
    )
    parser.add_argument(
        "--network",
        choices=("main", "testnet4", "signet"),
        default="main",
    )
    parser.add_argument("--target-peers", type=int, default=1)
    parser.add_argument(
        "--cli-command",
        default="bitcoin-cli",
        help="command prefix used to invoke bitcoin-cli",
    )
    args = parser.parse_args()

    await Sidecar(args).run()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("SIDECAR", "shutting down")
