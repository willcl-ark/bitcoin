from decimal import Decimal
from time import sleep

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error, get_fee


class TxFeeTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        # Fee rates in BTC/KvB
        # For sat/vB divide by 100
        self.min_tx_fee = Decimal("0.00001100")
        self.low_tx_fee = Decimal("0.00001000")
        self.test_tx_fee_too_low()

    def send_with_min_fee(self, sender, receiver):
        self.log.info(f'Starting send_with_min_fee')
        output = {receiver.getnewaddress(address_type='bech32'): 0.100000}
        raw_tx = sender.createrawtransaction(inputs=[], outputs=output)

        # set feeRate as per
        # https://github.com/bitcoin/bitcoin/issues/18475#issue-590603920
        funded_tx = sender.fundrawtransaction(raw_tx, {"feeRate": str(self.min_tx_fee)})

        signed_tx = sender.signrawtransactionwithwallet(funded_tx["hex"])
        decoded_tx = sender.decoderawtransaction(signed_tx["hex"])
        self.log.info(f'Signed transaction: {decoded_tx}')

        txid = sender.sendrawtransaction(signed_tx["hex"])
        self.log.info(f'Sent txid: {txid}')
        self.sync_mempools(timeout=10)

        self.generate(sender, 1)
        self.sync_mempools(timeout=10)
        self.log.info(f'Completed send_with_min_fee')

    def send_with_low_fee(self, sender, receiver):
        self.log.info(f'Starting send_with_low_fee')
        output = {receiver.getnewaddress(address_type='bech32'): 0.100000}
        raw_tx = sender.createrawtransaction(inputs=[], outputs=output)

        # set feeRate too low and fund
        funded_tx = sender.fundrawtransaction(raw_tx, {"feeRate": str(self.low_tx_fee)})

        # sign transaction
        signed_tx = sender.signrawtransactionwithwallet(funded_tx["hex"])
        decoded_tx = sender.decoderawtransaction(signed_tx["hex"])
        self.log.info(f'Signed transaction: {decoded_tx}')

        # try to send, permitted, but will fail to propagate
        txid = sender.sendrawtransaction(signed_tx["hex"])
        self.log.info(f'Sent txid: {txid}')
        sleep(5)
        assert_raises_rpc_error(-5, "Transaction not in mempool", receiver.getmempoolentry, txid)
        self.log.info(f'Transaction {txid} not entered receiver mempool')
        self.log.info(f'Completed send_with_low_fee')

    def test_tx_fee_too_low(self):
        # Restart nodes with individual args from
        # https://github.com/bitcoin/bitcoin/issues/18475#issue-590603920
        self.restart_node(0, extra_args=[f"-mintxfee={str(self.min_tx_fee)}"])
        self.restart_node(1, extra_args=[f"-minrelaytxfee={str(self.min_tx_fee)}"])
        self.connect_nodes(0, 1)
        self.generate(self.nodes[0], 101)

        # Test sending with minimum fee
        self.send_with_min_fee(self.nodes[0], self.nodes[1])

        # Test sending with a low fee
        self.send_with_low_fee(self.nodes[0], self.nodes[1])


if __name__ == '__main__':
    TxFeeTest().main()
