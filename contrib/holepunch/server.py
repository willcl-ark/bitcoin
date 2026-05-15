#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""TCP hole-punching rendezvous server for the raw proxy sidecar."""

import argparse
import asyncio
from dataclasses import dataclass
from datetime import datetime
import json
import random
import re
import secrets
import socket


CTRL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f]")


def sanitize(value):
    return CTRL_RE.sub("", str(value))


def log(tag, msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")
    print(f"{ts} {tag} {sanitize(msg)}", flush=True)


def normalize_addr(raw):
    ip, port = raw[:2]
    if ip.startswith("::ffff:"):
        ip = ip[7:]
    return ip, port


def fmt_addr(addr):
    ip, port = addr
    if ":" in ip:
        return f"[{ip}]:{port}"
    return f"{ip}:{port}"


def is_ipv6(ip):
    return ":" in ip


async def send_msg(writer, obj):
    try:
        writer.write(json.dumps(obj).encode("utf8") + b"\n")
        await writer.drain()
        return True
    except OSError:
        return False


async def read_msg(reader):
    line = await reader.readline()
    if not line:
        raise ConnectionError("connection closed")
    return json.loads(line)


@dataclass
class Client:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    addr: tuple[str, int]
    excluded_ips: set[str]

    @property
    def family(self):
        return "v6" if is_ipv6(self.addr[0]) else "v4"


class RendezvousServer:
    def __init__(self, match_interval):
        self.match_interval = match_interval
        self.waiting = {"v4": [], "v6": []}

    async def handle_client(self, reader, writer):
        addr = normalize_addr(writer.get_extra_info("peername"))
        tag = f"CLIENT[{fmt_addr(addr)}]"
        log(tag, f"connected ({'v6' if is_ipv6(addr[0]) else 'v4'})")

        if not await send_msg(writer, {"type": "welcome", "you": list(addr)}):
            writer.close()
            await writer.wait_closed()
            return
        try:
            msg = await read_msg(reader)
        except (ConnectionError, json.JSONDecodeError) as e:
            writer.close()
            await writer.wait_closed()
            log(tag, f"closed before lobby join: {e}")
            return
        if msg.get("type") != "exclude":
            writer.close()
            await writer.wait_closed()
            log(tag, f"closed with unexpected lobby message: {msg}")
            return

        excluded_ips = {ip for ip in msg.get("ips", []) if isinstance(ip, str)}
        client = Client(
            reader=reader,
            writer=writer,
            addr=addr,
            excluded_ips=excluded_ips,
        )
        log(tag, f"joined ({client.family}) excluding {len(excluded_ips)} peer(s)")
        self.waiting[client.family].append(client)
        try:
            while await reader.readline():
                pass
        finally:
            if client in self.waiting[client.family]:
                self.waiting[client.family].remove(client)
            writer.close()
            await writer.wait_closed()
            log(tag, "closed")

    async def match_loop(self):
        while True:
            await asyncio.sleep(self.match_interval)
            for family in ("v4", "v6"):
                await self.match_family(family)

    async def match_family(self, family):
        live = [c for c in self.waiting[family] if not c.writer.is_closing()]
        self.waiting[family] = live
        if len(live) < 2:
            for client in live:
                await send_msg(client.writer, {"type": "wait"})
            return

        matched = []
        unmatched = live.copy()
        random.shuffle(unmatched)
        while len(unmatched) >= 2:
            left = unmatched.pop()
            right_index = next(
                (i for i, right in enumerate(unmatched) if self.can_pair(left, right)),
                None,
            )
            if right_index is None:
                continue
            right = unmatched.pop(right_index)

            match_id = secrets.token_hex(16)
            log(
                f"POOL[{family}]",
                f"pair {fmt_addr(left.addr)} <-> {fmt_addr(right.addr)}",
            )
            await send_msg(
                left.writer,
                {
                    "type": "punch",
                    "match": match_id,
                    "role": "initiator",
                    "peer": list(right.addr),
                },
            )
            await send_msg(
                right.writer,
                {
                    "type": "punch",
                    "match": match_id,
                    "role": "receiver",
                    "peer": list(left.addr),
                },
            )
            matched.append(left)
            matched.append(right)

        self.waiting[family] = [c for c in live if c not in matched]
        for client in matched:
            client.writer.close()
        for client in self.waiting[family]:
            await send_msg(client.writer, {"type": "wait"})

    def can_pair(self, left, right):
        return (
            left.addr[0] not in right.excluded_ips
            and right.addr[0] not in left.excluded_ips
        )


def create_server_socket(args):
    if args.ipv4:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", args.port))
        log("LISTEN", f"0.0.0.0:{args.port} (IPv4)")
    elif args.ipv6:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        sock.bind(("::", args.port))
        log("LISTEN", f"[::]:{args.port} (IPv6)")
    else:
        try:
            sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            sock.bind(("::", args.port))
            log("LISTEN", f"[::]:{args.port} (dual-stack)")
        except OSError:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("", args.port))
            log("LISTEN", f"0.0.0.0:{args.port} (IPv4 fallback)")
    sock.listen(128)
    sock.setblocking(False)
    return sock


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("-4", dest="ipv4", action="store_true", help="IPv4 only")
    group.add_argument("-6", dest="ipv6", action="store_true", help="IPv6 only")
    parser.add_argument("port", nargs="?", type=int, default=57996)
    parser.add_argument("--match-interval", type=float, default=5.0)
    args = parser.parse_args()

    rendezvous = RendezvousServer(match_interval=args.match_interval)
    server = await asyncio.start_server(
        rendezvous.handle_client,
        sock=create_server_socket(args),
    )
    async with server:
        asyncio.create_task(rendezvous.match_loop())
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("LISTEN", "shutting down")
