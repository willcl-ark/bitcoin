#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared utilities for IPC interface tests."""
import asyncio
import inspect
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
import shutil
from typing import Optional

from test_framework.messages import CBlock
from test_framework.util import (
    assert_equal
)

# Test may be skipped and not have capnp installed
try:
    import capnp  # type: ignore[import] # noqa: F401
except ModuleNotFoundError:
    pass


# Stores the result of getCoinbaseTx()
@dataclass
class CoinbaseTxData:
    version: int
    sequence: int
    scriptSigPrefix: bytes
    witness: Optional[bytes]
    blockRewardRemaining: int
    requiredOutputs: list[bytes]
    lockTime: int


def optional_value(result):
    if result.which() == "none":
        return None
    assert_equal(result.which(), "value")
    return result.value


async def wait_and_do(wait_fn, do_fn):
    """Call wait_fn, then sleep, then call do_fn in a parallel task. Wait for
    both tasks to complete."""
    wait_started = asyncio.Event()
    result = None

    async def wait():
        nonlocal result
        wait_started.set()
        result = await wait_fn

    async def do():
        await wait_started.wait()
        await asyncio.sleep(0.1)
        # Let do_fn be either a callable or an awaitable object
        if inspect.isawaitable(do_fn):
            await do_fn
        else:
            do_fn()

    await asyncio.gather(wait(), do())
    return result


def load_capnp_modules(config):
    if capnp_bin := shutil.which("capnp"):
        # Add the system cap'nproto path so include/capnp/c++.capnp can be found.
        capnp_dir = Path(capnp_bin).resolve().parent.parent / "include"
    else:
        # If there is no system cap'nproto, the pycapnp module should have its own "bundled"
        # includes at this location. If pycapnp was installed with bundled capnp,
        # capnp/c++.capnp can be found here.
        capnp_dir = Path(capnp.__path__[0]).parent
    src_dir = Path(config['environment']['SRCDIR']) / "src"
    imports = [str(capnp_dir), str(src_dir)]
    return {
        "init": capnp.load(str(src_dir / "ipc" / "capnp" / "init.capnp"), imports=imports),
        "echo": capnp.load(str(src_dir / "ipc" / "capnp" / "echo.capnp"), imports=imports),
        "mining": capnp.load(str(src_dir / "ipc" / "capnp" / "mining.capnp"), imports=imports),
    }


async def make_capnp_init(self):
    """Establish a connection and return an Init client."""
    node = self.nodes[0]
    connection = await capnp.AsyncIoStream.create_unix_connection(node.ipc_socket_path)
    client = capnp.TwoPartyClient(connection)
    return client.bootstrap().cast_as(self.capnp_modules['init'].Init)


async def mining_create_block_template(mining, *args, **kwargs):
    """Call mining.createNewBlock() and return template."""
    response = await mining.createNewBlock(*args, **kwargs)
    return optional_value(response.result)


async def mining_wait_next_template(template, opts):
    """Call template.waitNext() and return template."""
    response = await template.waitNext(opts)
    return optional_value(response.result)


async def mining_get_block(block_template):
    block_data = BytesIO((await block_template.getBlock()).result)
    block = CBlock()
    block.deserialize(block_data)
    return block


async def mining_get_coinbase_tx(block_template) -> CoinbaseTxData:
    assert block_template is not None
    # Note: the template_capnp struct will be garbage-collected when this
    # method returns, so it is important to copy any Data fields from it
    # which need to be accessed later using the bytes() cast. Starting with
    # pycapnp v2.2.0, Data fields have type `memoryview` and are ephemeral.
    template_capnp = (await block_template.getCoinbaseTx()).result
    witness: Optional[bytes] = None
    if template_capnp._has("witness"):
        witness = bytes(template_capnp.witness)
    return CoinbaseTxData(
        version=int(template_capnp.version),
        sequence=int(template_capnp.sequence),
        scriptSigPrefix=bytes(template_capnp.scriptSigPrefix),
        witness=witness,
        blockRewardRemaining=int(template_capnp.blockRewardRemaining),
        requiredOutputs=[bytes(output) for output in template_capnp.requiredOutputs],
        lockTime=int(template_capnp.lockTime),
    )


async def make_capnp_mining(self):
    """Create a Mining client."""
    init = await make_capnp_init(self)
    self.log.debug("Create Mining client")
    return init.makeMining().result


def assert_capnp_failed(e, description_prefix):
    assert e.description.startswith(description_prefix), f"Expected description starting with '{description_prefix}', got '{e.description}'"
    assert_equal(e.type, "FAILED")
