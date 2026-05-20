#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the IPC (multiprocess) interface."""
import asyncio
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.ipc_util import (
    load_capnp_modules,
    make_capnp_init,
    make_capnp_mining,
    optional_value,
)

# Test may be skipped and not have capnp installed
try:
    import capnp  # type: ignore[import] # noqa: F401
except ModuleNotFoundError:
    pass


class IPCInterfaceTest(BitcoinTestFramework):

    def skip_test_if_missing_module(self):
        self.skip_if_no_ipc()
        self.skip_if_no_py_capnp()

    def set_test_params(self):
        self.num_nodes = 1

    def setup_nodes(self):
        self.extra_init = [{"ipcbind": True}]
        super().setup_nodes()
        # Use this function to also load the capnp modules (we cannot use set_test_params for this,
        # as it is being called before knowing whether capnp is available).
        self.capnp_modules = load_capnp_modules(self.config)

    def run_echo_test(self):
        self.log.info("Running echo test")
        async def async_routine():
            init = await make_capnp_init(self)
            self.log.debug("Create Echo client")
            echo = init.makeEcho().result
            self.log.debug("Test a few invocations of echo")
            for s in ["hallo", "", "haha"]:
                result_eval = (await echo.echo(s)).result
                assert_equal(s, result_eval)
        asyncio.run(capnp.run(async_routine()))

    def run_mining_test(self):
        self.log.info("Running mining test")
        block_hash_size = 32

        async def async_routine():
            mining = await make_capnp_mining(self)
            self.log.debug("Test simple inspectors")
            assert (await mining.isTestChain()).result
            assert not (await mining.isInitialBlockDownload()).result
            blockref = optional_value((await mining.getTip()).result)
            assert blockref is not None
            assert_equal(len(blockref.hash), block_hash_size)
            current_block_height = self.nodes[0].getchaintips()[0]["height"]
            assert_equal(blockref.height, current_block_height)

        asyncio.run(capnp.run(async_routine()))

    def run_unclean_disconnect_test(self):
        """Test behavior when disconnecting during an IPC call that later
        returns a non-null interface pointer. This used to cause a crash as
        reported https://github.com/bitcoin/bitcoin/issues/34250, but now just
        results in a cancellation log message"""
        node = self.nodes[0]
        self.log.info("Running disconnect during BlockTemplate.waitNext")
        timeout = self.rpc_timeout * 1000.0

        async def async_routine():
            mining = await make_capnp_mining(self)
            self.log.debug("Create a template")
            opts = self.capnp_modules['mining'].BlockCreateOptions()
            template = optional_value((await mining.createNewBlock(opts)).result)
            assert template is not None

            self.log.debug("Wait for a new template")
            waitoptions = self.capnp_modules['mining'].BlockWaitOptions()
            waitoptions.timeout = timeout
            waitoptions.feeThreshold = 1
            promise = template.waitNext(waitoptions)
            await asyncio.sleep(0.1)
            del promise

        asyncio.run(capnp.run(async_routine()))

        self.generate(node, 1)

        async def fresh_connection_check():
            mining = await make_capnp_mining(self)
            assert (await mining.isTestChain()).result

        asyncio.run(capnp.run(fresh_connection_check()))

    def run_thread_busy_test(self):
        """Test behavior when sending multiple calls to the same server thread
        which used to cause a crash as reported
        https://github.com/bitcoin/bitcoin/issues/33923."""
        node = self.nodes[0]
        self.log.info("Running thread busy test")
        timeout = self.rpc_timeout * 1000.0

        async def async_routine():
            mining = await make_capnp_mining(self)
            self.log.debug("Create a template")
            opts = self.capnp_modules['mining'].BlockCreateOptions()
            template = optional_value((await mining.createNewBlock(opts)).result)
            assert template is not None

            self.log.debug("Wait for a new template")
            waitoptions = self.capnp_modules['mining'].BlockWaitOptions()
            waitoptions.timeout = timeout
            waitoptions.feeThreshold = 1

            promise1 = template.waitNext(waitoptions)
            await asyncio.sleep(0.1)
            promise2 = template.waitNext(waitoptions)
            await asyncio.sleep(0.1)
            promise3 = template.waitNext(waitoptions)
            await asyncio.sleep(0.1)

            # Generate a new block to make the active waitNext calls return, then clean up.
            self.generate(node, 1, sync_fun=self.no_op)
            assert optional_value((await promise1).result) is not None
            assert optional_value((await promise2).result) is not None
            assert optional_value((await promise3).result) is not None

        asyncio.run(capnp.run(async_routine()))

    def run_test(self):
        self.run_echo_test()
        self.run_mining_test()
        self.run_unclean_disconnect_test()
        self.run_thread_busy_test()

if __name__ == '__main__':
    IPCInterfaceTest(__file__).main()
