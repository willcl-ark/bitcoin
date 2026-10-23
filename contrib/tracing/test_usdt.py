#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check real USDT payloads without BPF or root.

Run after building with WITH_USDT=ON:
  python3 contrib/tracing/test_usdt.py --configfile=build/test/config.ini

Requires readelf and GDB with Python and SystemTap probe support. GDB launches
the node as its child, activates probe semaphores, and copies arguments while stopped.
The RPC/P2P driver runs separately so it never issues RPCs to a stopped node.
Timing checks are bounds, not benchmarks; cache memory gets a sanity check.
"""

import json
from pathlib import Path
import sys


# Integers are decoded using the signedness/width in the actual USDT metadata.
# Strings, hashes and message buffers must be copied before the node resumes.
# Field kinds: integer (i), C string (s), 32-byte hash (h), message bytes (b).
PROBES = {
    "net:inbound_message": "isssib",
    "net:outbound_message": "isssib",
    "net:inbound_connection": "issii",
    "net:outbound_connection": "issii",
    "net:closed_connection": "issii",
    "net:evicted_inbound_connection": "issii",
    "net:misbehaving_connection": "is",
    "mempool:added": "hii",
    "mempool:removed": "hsiii",
    "mempool:replaced": "hiiihiii",
    "mempool:rejected": "hs",
    "validation:block_connected": "hiiiii",
    "utxocache:add": "hiiii",
    "utxocache:spent": "hiiii",
    "utxocache:uncache": "hiiii",
    "utxocache:flush": "iiiii",
    "coin_selection:selected_coins": "ssiii",
    "coin_selection:normal_create_tx_internal": "siii",
    "coin_selection:attempting_aps_create_tx": "s",
    "coin_selection:aps_create_tx_internal": "siiii",
}


def record_probes():
    import gdb
    import re
    import subprocess

    output = open(globals()["events_path"], "a", buffering=1)
    char_pointer = gdb.lookup_type("char").pointer()

    def read_operand(description):
        # GDB 17.2 rejects GCC's r8b..r15b names, calling them r8l..r15l.
        # Read the unmodified ELF operand only for that specific parser error.
        size, operand = description.split("@", 1)
        size = int(size)
        arg_type = gdb.lookup_type(("signed " if size < 0 else "unsigned ") +
                                   {1: "char", 2: "short", 4: "int", 8: "long long"}[abs(size)])
        if operand.startswith("%"):
            register = re.sub(r"^(r(?:[89]|1[0-5]))b$", r"\1l", operand[1:])
            value = gdb.parse_and_eval(f"${register}")
        elif operand.startswith("$"):
            value = gdb.Value(int(operand[1:], 0))
        else:
            address = re.fullmatch(r"(-?\d*)\(%(\w+)\)", operand)
            if address is None:
                raise AssertionError(f"Unsupported USDT operand: {description}")
            offset, register = address.groups()
            value = gdb.Value(int(gdb.parse_and_eval(f"${register}")) + int(offset or 0)).cast(arg_type.pointer()).dereference()
        return int(value.cast(arg_type))

    class Probe(gdb.Breakpoint):
        def __init__(self, name, fields):
            super().__init__(f"-probe-stap {name}", internal=True)
            self.probe_name = name
            self.fields = fields
            self.operands = None

        def stop(self):
            try:
                try:
                    count = int(gdb.parse_and_eval("$_probe_argc"))
                    raw = [int(gdb.parse_and_eval(f"$_probe_arg{i}")) for i in range(count)]
                except gdb.error as error:
                    if not re.match(r"Invalid register name .r(?:[89]|1[0-5])b", str(error)):
                        raise
                    if self.operands is None:
                        notes = subprocess.check_output(["readelf", "--notes", gdb.current_progspace().filename], text=True)
                        layouts = {f"{provider}:{name}": args.split() for provider, name, args in
                                   re.findall(r"Provider: (\w+)\n\s+Name: (\w+).*?Arguments: ([^\n]*)", notes, re.S)}
                        self.operands = layouts[self.probe_name]
                    raw = [read_operand(operand) for operand in self.operands]
                    count = len(raw)
                if count != len(self.fields):
                    raise AssertionError(f"{self.probe_name}: expected {len(self.fields)} arguments, got {count}")
                args = []
                for field, value in zip(self.fields, raw):
                    if field == "s":
                        value = gdb.Value(value).cast(char_pointer).string(encoding="utf-8")
                    elif field == "h":
                        value = bytes(gdb.selected_inferior().read_memory(value, 32))[::-1].hex()
                    elif field == "b":
                        message_size = raw[4]
                        # Empty messages, e.g. verack, have no bytes to read.
                        # GDB's read_memory requires a strictly positive size.
                        if message_size == 0:
                            value = ""
                        else:
                            value = bytes(gdb.selected_inferior().read_memory(value, message_size)).hex()
                    args.append(value)
                output.write(json.dumps({"probe": self.probe_name, "args": args}) + "\n")
                return False
            except Exception as error:
                message = f"{self.probe_name}: {error}"
                output.write(json.dumps({"error": message}) + "\n")
                gdb.write(message + "\n", gdb.STDERR)
                return True

    for name, fields in PROBES.items():
        Probe(name, fields)


def run_test():
    from decimal import Decimal
    import shutil
    import time

    functional_dir = Path(__file__).resolve().parents[2] / "test" / "functional"
    sys.path.insert(0, str(functional_dir))

    from test_framework.address import address_to_scriptpubkey
    from test_framework.messages import COIN, CBlockHeader, msg_headers, msg_ping, msg_pong
    from test_framework.p2p import P2PDataStore, P2PInterface
    from test_framework.test_framework import BitcoinTestFramework
    from test_framework.util import assert_equal, assert_greater_than
    from test_framework.wallet import MiniWallet

    class PayloadTest(BitcoinTestFramework):
        def set_test_params(self):
            self.num_nodes = 1
            self.wallet_names = [False]
            self.supports_cli = False
            self.extra_args = [["-maxconnections=53", "-checkmempool=0"]]
            self.checked = set()
            self.offset = 0

        def skip_test_if_missing_module(self):
            self.skip_if_no_wallet()
            if not shutil.which("gdb"):
                raise RuntimeError("Install GDB with Python and SystemTap probe support")

        def add_options(self, parser):
            parser.set_defaults(cachedir=str(functional_dir.parents[1] / "build" / "test" / "cache"))

        def setup_nodes(self):
            self.add_nodes(self.num_nodes, self.extra_args)
            self.events_path = Path(self.options.tmpdir) / "probes.jsonl"
            script = Path(self.options.tmpdir) / "capture.gdb"
            script.write_text(
                "set pagination off\nset confirm off\nset debuginfod enabled off\n"
                "set startup-with-shell off\nset disable-randomization off\n"
                "set print thread-events off\npython\nimport runpy\n"
                f"runpy.run_path({str(Path(__file__).resolve())!r}, "
                f"init_globals={{'events_path': {str(self.events_path)!r}}}, run_name='__gdb__')\n"
                "end\nrun\n"
            )
            self.nodes[0].args = ["gdb", "-q", "-nx", "--batch", "--return-child-result", "-x", str(script), "--args", *self.nodes[0].args]
            self.start_nodes()

        def records(self):
            if not self.events_path.exists():
                return []
            # A callback may be writing the last line while the driver reads.
            rows = [json.loads(line) for line in self.events_path.read_text().splitlines(keepends=True) if line.endswith("\n")]
            for row in rows:
                if "error" in row:
                    raise AssertionError(row["error"])
            return rows

        def checkpoint(self):
            self.offset = len(self.records())

        def captured(self, name):
            return [row["args"] for row in self.records()[self.offset:] if row["probe"] == name]

        def event(self, name, key=None):
            def matching():
                return [args for args in self.captured(name) if key is None or args[0] == key]
            self.wait_until(lambda: bool(matching()))
            self.checked.add(name)
            return matching()[0]

        def expect(self, name, args):
            try:
                self.wait_until(lambda: args in self.captured(name), timeout=10)
            except AssertionError as error:
                raise AssertionError(f"{name}: expected {args}, captured {self.captured(name)}") from error
            self.checked.add(name)

        def test_cache(self):
            node = self.nodes[0]
            self.restart_node(0)
            self.generate(node, 1)
            node.setmocktime(node.getblockheader(node.getbestblockhash())["time"] + 1)
            self.checkpoint()
            node.gettxoutsetinfo("none", use_index=False)
            flush = self.event("utxocache:flush")
            # Only the newly mined coin is cached after restarting. Mock time
            # is fixed, so NodeClock measures exactly zero microseconds.
            assert_equal(flush[:3], [0, 4, 1])
            assert_greater_than(flush[3], 0)
            assert_equal(flush[4], 0)
            node.setmocktime(0)

            self.wallet = MiniWallet(node)
            first_coinbase = node.getblock(node.getblockhash(1))["tx"][0]
            invalid = self.wallet.create_self_transfer()["tx"]
            invalid.vin[0].prevout.hash = int(first_coinbase, 16)
            self.checkpoint()
            assert_equal(node.testmempoolaccept([invalid.serialize().hex()])[0]["allowed"], False)
            self.expect("utxocache:uncache", [first_coinbase, 0, 1, 50 * COIN, 1])

        def test_transactions(self):
            node = self.nodes[0]
            utxo = self.wallet.get_utxo()
            self.checkpoint()
            original = self.wallet.send_self_transfer(from_node=node, utxo_to_spend=utxo, fee=Decimal(40000) / COIN)
            entry_time = node.getmempoolentry(original["txid"])["time"]
            self.expect("mempool:added", [original["txid"], original["tx"].get_vsize(), 40000])
            replacement = self.wallet.send_self_transfer(from_node=node, utxo_to_spend=utxo, fee=Decimal(45000) / COIN)
            self.expect("mempool:removed", [original["txid"], "replaced", original["tx"].get_vsize(), 40000, entry_time])
            self.expect("mempool:replaced", [original["txid"], original["tx"].get_vsize(), 40000, entry_time,
                                            replacement["txid"], replacement["tx"].get_vsize(), 45000, 1])

            peer = node.add_p2p_connection(P2PDataStore())
            rejected = self.wallet.create_self_transfer(fee_rate=0)
            peer.send_txs_and_test([rejected["tx"]], node, success=False)
            self.expect("mempool:rejected", [rejected["txid"], "min relay fee not met"])
            node.disconnect_p2ps()

            self.checkpoint()
            before = time.monotonic_ns()
            block_hash = self.generate(node, 1)[0]
            elapsed = time.monotonic_ns() - before
            block = node.getblock(block_hash, 2)
            connected = self.event("validation:block_connected", block_hash)
            # The generated coinbase pays P2PKH: one legacy CHECKSIG costs
            # four sigop units. The MiniWallet OP_TRUE spend adds none.
            assert_equal(connected[:5], [block_hash, block["height"], len(block["tx"]), sum(len(tx["vin"]) for tx in block["tx"]), 4])
            assert 0 < connected[5] <= elapsed
            for tx in block["tx"]:
                for output in tx["vout"]:
                    # Unspendable outputs, including the witness commitment,
                    # never enter the UTXO cache.
                    if output["scriptPubKey"]["type"] == "nulldata":
                        continue
                    self.expect("utxocache:add", [tx["txid"], output["n"], block["height"], int(output["value"] * COIN), int("coinbase" in tx["vin"][0])])
            self.expect("utxocache:spent", [utxo["txid"], utxo["vout"], utxo["height"], int(utxo["value"] * COIN), int(utxo["coinbase"])])
            self.wallet.rescan_utxos()

        def test_selection(self):
            node = self.nodes[0]
            name = "usdt"
            node.createwallet(name)
            wallet = node.get_wallet_rpc(name)
            funding = self.wallet.send_to(from_node=node, scriptPubKey=address_to_scriptpubkey(wallet.getnewaddress()), amount=COIN)
            self.generate(node, 1)
            outpoint = {"txid": funding["txid"], "vout": funding["sent_vout"]}
            raw = node.createrawtransaction([outpoint], {wallet.getnewaddress(): Decimal(1)})
            self.checkpoint()
            funded = wallet.fundrawtransaction(raw, {"add_inputs": False, "subtractFeeFromOutputs": [0], "fee_rate": 10,
                                                     "input_weights": [dict(outpoint, weight=400)]})
            tx = node.decoderawtransaction(funded["hex"])
            fee = COIN - sum(int(output["value"] * COIN) for output in tx["vout"])
            assert_equal(int(funded["fee"] * COIN), fee)
            assert_equal(funded["changepos"], -1)
            # Manual input: 400 weight units = 100 vbytes, at 10 sat/vbyte.
            # There is no change/excess, and preset inputs have zero long-term fee.
            for selected in self.captured("coin_selection:selected_coins"):
                assert_equal(selected, [name, "manual", COIN, 1000, COIN])
            self.expect("coin_selection:selected_coins", [name, "manual", COIN, 1000, COIN])
            self.expect("coin_selection:normal_create_tx_internal", [name, 1, fee, -1])
            self.expect("coin_selection:attempting_aps_create_tx", [name])
            self.expect("coin_selection:aps_create_tx_internal", [name, 1, 1, fee, -1])

        def test_network(self):
            node = self.nodes[0]
            self.checkpoint()
            peer = node.add_p2p_connection(P2PInterface())
            info = node.getpeerinfo()[0]
            identity = [info["id"], info["addr"], "inbound", 0]
            self.expect("net:inbound_connection", identity + [1])
            self.expect("net:inbound_message", identity[:3] + ["verack", 0, ""])
            self.expect("net:outbound_message", identity[:3] + ["verack", 0, ""])
            self.checkpoint()
            ping = msg_ping(0x123456789ABCDEF)
            peer.send_without_ping(ping)
            self.wait_until(lambda: peer.last_message.get("pong", msg_pong()).nonce == ping.nonce)
            self.expect("net:inbound_message", identity[:3] + ["ping", len(ping.serialize()), ping.serialize().hex()])
            pong = msg_pong(ping.nonce)
            self.expect("net:outbound_message", identity[:3] + ["pong", len(pong.serialize()), pong.serialize().hex()])
            node.disconnect_p2ps()
            self.expect("net:closed_connection", identity + [info["conntime"]])

            self.checkpoint()
            node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0, connection_type="outbound-full-relay")
            info = node.getpeerinfo()[0]
            self.expect("net:outbound_connection", [info["id"], info["addr"], "outbound-full-relay", 0, 1])
            node.disconnect_p2ps()

            # With maxconnections=53, 21 inbound tx-relaying slots are available;
            # the 22nd peer evicts one of the previous 21.
            for _ in range(21):
                node.add_p2p_connection(P2PInterface())
            previous = {info["id"]: info for info in node.getpeerinfo()}
            self.checkpoint()
            node.add_p2p_connection(P2PInterface())
            evicted = self.event("net:evicted_inbound_connection")
            self.wait_until(lambda: evicted[0] not in {info["id"] for info in node.getpeerinfo()})
            info = previous[evicted[0]]
            assert_equal(evicted, [info["id"], info["addr"], "inbound", 0, info["conntime"]])
            node.disconnect_p2ps()

            self.checkpoint()
            peer = node.add_p2p_connection(P2PInterface())
            info = node.getpeerinfo()[0]
            peer.send_without_ping(msg_headers([CBlockHeader()] * 2001))
            self.expect("net:misbehaving_connection", [info["id"], "headers message size = 2001"])
            node.disconnect_p2ps()

        def run_test(self):
            try:
                for test in [self.test_cache, self.test_transactions, self.test_selection, self.test_network]:
                    self.log.info(test.__name__)
                    test()
                assert_equal(self.checked, set(PROBES))
                self.log.info("Checked payloads at all %d USDT sites", len(self.checked))
            finally:
                # Surface capture failures instead of the resulting RPC disconnect.
                self.records()
                self.stop_nodes()

    PayloadTest(str(functional_dir / "usdt_payloads.py")).main()


if __name__ == "__gdb__":
    record_probes()
elif __name__ == "__main__":
    run_test()
