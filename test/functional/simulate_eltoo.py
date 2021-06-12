#!/usr/bin/env python3
# Copyright (c) 2015-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Simulation tests for eltoo payment channel update scheme
"""

from collections import OrderedDict, namedtuple
import copy
from io import BytesIO
import json
from test_framework.address import program_to_witness
from test_framework.blocktools import (
    create_block,
    create_coinbase,
    WITNESS_SCALE_FACTOR
)
from test_framework.key import (
    compute_xonly_pubkey,
    generate_privkey,
    sign_schnorr,
    tweak_add_pubkey,
    ECKey,
    ECPubKey,
    SECP256K1_ORDER
)
from test_framework.messages import (
    ser_string,
    sha256,
    COutPoint,
    CScriptWitness,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    ToHex
)
from test_framework.p2p import P2PDataStore
from test_framework.script import (
    hash160,
    hash256,
    sha256,
    taproot_construct,
    taproot_tree_helper,
    CScript,
    CScriptNum,
    KEY_VERSION_TAPROOT,
    KEY_VERSION_ANYPREVOUT,
    LEAF_VERSION_TAPSCRIPT,
    OP_0,
    OP_1,
    OP_2,
    OP_2DUP,
    OP_3DUP,
    OP_2DROP,
    OP_CHECKLOCKTIMEVERIFY,
    OP_CHECKMULTISIG,
    OP_CHECKMULTISIGVERIFY,
    OP_CHECKSEQUENCEVERIFY,
    OP_CHECKSIG,
    OP_CHECKSIGADD,
    OP_CHECKSIGVERIFY,
    OP_DROP,
    OP_DUP,
    OP_ELSE,
    OP_ENDIF,
    OP_EQUAL,
    OP_EQUALVERIFY,
    OP_FALSE,
    OP_HASH160,
    OP_IF,
    OP_INVALIDOPCODE,
    OP_NOTIF,
    OP_NUMEQUAL,
    OP_RETURN,
    OP_TRUE,
    OP_VERIFY,
    SIGHASH_DEFAULT,
    SIGHASH_ALL,
    SIGHASH_ANYPREVOUT,
    SIGHASH_NONE,
    SIGHASH_SINGLE,
    SIGHASH_ANYONECANPAY,
    SIGHASH_ANYPREVOUT,
    SIGHASH_ANYPREVOUTANYSCRIPT,
    LegacySignatureHash,
    TaprootSignatureHash
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_raises_rpc_error
)

import time
import random

NUM_OUTPUTS_TO_COLLECT = 33
CSV_DELAY = 20
DUST_LIMIT = 600
FEE_AMOUNT = 1000
CHANNEL_AMOUNT = 1000000
RELAY_FEE = 100
NUM_SIGNERS = 2
CLTV_START_TIME = 500000000
INVOICE_TIMEOUT = 3600  # 60 minute
BLOCK_TIME = 600  # 10 minutes
MIN_FEE = 50000
DEFAULT_NSEQUENCE = 0xFFFFFFFE  # disable nSequence lock


# from bitcoinops script.py
def get_version_tagged_pubkey(pubkey, version):
    assert pubkey.is_compressed
    assert pubkey.is_valid
    # When the version 0xfe is used, the control block may become indistinguishable from annex.
    # In such case, use of annex becomes mandatory.
    assert 0 <= version < 0xff and not (version & 1)
    data = pubkey.get_bytes()
    return bytes([data[0] & 1 | version]) + data[1:]


# from bitcoinops util.py
def create_spending_transaction(node, txid, version=1, nSequence=0, nLockTime=0):
    """Construct a CTransaction object that spends the first ouput from txid."""
    # Construct transaction
    spending_tx = CTransaction()

    # Populate the transaction version
    spending_tx.nVersion = version

    # Populate the locktime
    spending_tx.nLockTime = nLockTime

    # Populate the transaction inputs
    outpoint = COutPoint(int(txid, 16), 0)
    spending_tx_in = CTxIn(outpoint=outpoint, nSequence=nSequence)
    spending_tx.vin = [spending_tx_in]
    dest_addr = node.getnewaddress(address_type="bech32")
    scriptpubkey = bytes.fromhex(node.getaddressinfo(dest_addr)['scriptPubKey'])

    # Complete output which returns 0.5 BTC to Bitcoin Core wallet
    amount_sat = int(0.5 * 100_000_000)
    dest_output = CTxOut(nValue=amount_sat, scriptPubKey=scriptpubkey)
    spending_tx.vout = [dest_output]

    return spending_tx


# from bitcoinops util.py
def generate_and_send_coins(node, address, amount_sat):
    """Generate blocks on node and then send amount_sat to address.
    No change output is added to the transaction.
    Return a CTransaction object."""
    version = node.getnetworkinfo()['subversion']
    print("\nClient version is {}\n".format(version))

    # Generate 101 blocks and send reward to bech32 address
    reward_address = node.getnewaddress(address_type="bech32")
    node.generatetoaddress(101, reward_address)
    balance = node.getbalance()
    print("Balance: {}\n".format(balance))

    assert balance > 1

    unspent_txid = node.listunspent(1)[-1]["txid"]
    inputs = [{"txid": unspent_txid, "vout": 0}]

    # Create a raw transaction sending 1 BTC to the address, then sign and send it.
    # We won't create a change output, so maxfeerate must be set to 0
    # to allow any fee rate.
    tx_hex = node.createrawtransaction(inputs=inputs, outputs=[{address: amount_sat / 100_000_000}])

    res = node.signrawtransactionwithwallet(hexstring=tx_hex)

    tx_hex = res["hex"]
    assert res["complete"]
    assert 'errors' not in res

    txid = node.sendrawtransaction(hexstring=tx_hex, maxfeerate=0)

    tx_hex = node.getrawtransaction(txid)

    # Reconstruct wallet transaction locally
    tx = CTransaction()
    tx.deserialize(BytesIO(bytes.fromhex(tx_hex)))
    tx.rehash()

    return tx


# from bitcoinops util.py
def test_transaction(node, tx):
    tx_str = tx.serialize().hex()
    ret = node.testmempoolaccept(rawtxs=[tx_str], maxfeerate=0)[0]
    print(ret)
    return ret['allowed']


def flatten(lst):
    ret = []
    for elem in lst:
        if isinstance(elem, list):
            ret += flatten(elem)
        else:
            ret.append(elem)
    return ret


def dump_json_test(tx, input_utxos, idx, success, failure):
    spender = input_utxos[idx].spender

    fields = [
        ("tx", tx.serialize().hex()),
        ("prevouts", [x.output.serialize().hex() for x in input_utxos]),
        ("index", idx)
    ]

    def dump_witness(wit):
        return OrderedDict([("scriptSig", wit[0].hex()), ("witness", [x.hex() for x in wit[1]])])

    if success is not None:
        fields.append(("success", dump_witness(success)))
    if failure is not None:
        fields.append(("failure", dump_witness(failure)))

    # Write the dump to $TEST_DUMP_DIR/x/xyz... where x,y,z,... are the SHA1 sum of the dump (which makes the
    # file naming scheme compatible with fuzzing infrastructure).
    dump = json.dumps(OrderedDict(fields)) + ",\n"
    print(dump)


def int_to_bytes(x) -> bytes:
    return x.to_bytes((x.bit_length() + 7) // 8, 'big')


def get_state_address(inner_pubkey, state):
    update_script = get_update_tapscript(state)
    settle_script = get_settle_tapscript()
    taptree = taproot_construct(inner_pubkey, [
        ("update", update_script), ("settle", settle_script)
    ])
    tweaked, _ = tweak_add_pubkey(taptree.inner_pubkey, taptree.tweak)
    address = program_to_witness(version=0x01, program=tweaked, main=False)
    return address


def get_htlc_address(inner_pubkey, preimage_hash, claim_pubkey, expiry, refund_pubkey):
    htlc_claim_script = get_htlc_claim_tapscript(preimage_hash, claim_pubkey)
    htlc_refund_script = get_htlc_refund_tapscript(expiry, refund_pubkey)
    taptree = taproot_construct(inner_pubkey, [
        ("htlc_claim", htlc_claim_script), ("htlc_refund", htlc_refund_script)
    ])
    tweaked, _ = tweak_add_pubkey(taptree.inner_pubkey, taptree.tweak)
    address = program_to_witness(version=0x01, program=tweaked, main=False)
    return address


def create_update_transaction(node, source_tx, dest_addr, state, amount_sat):
    # UPDATE TX
    # nlocktime: CLTV_START_TIME + state
    # nsequence: 0
    # sighash=SINGLE | ANYPREVOUTANYSCRIPT
    update_tx = CTransaction()
    update_tx.nVersion = 2
    update_tx.nLockTime = CLTV_START_TIME + state

    # Populate the transaction inputs
    source_tx.rehash()
    outpoint = COutPoint(int(source_tx.hash, 16), 0)
    update_tx.vin = [CTxIn(outpoint=outpoint, nSequence=0)]

    scriptpubkey = bytes.fromhex(node.getaddressinfo(dest_addr)['scriptPubKey'])
    dest_output = CTxOut(nValue=amount_sat, scriptPubKey=scriptpubkey)
    update_tx.vout = [dest_output]

    return update_tx


def create_settle_transaction(node, source_tx, outputs):
    # SETTLE TX
    # nlocktime: CLTV_START_TIME + state + 1
    # nsequence: CSV_DELAY
    # sighash=SINGLE | ANYPREVOUT (using ANYPREVOUT commits to a specific state because 'n' in the update leaf is commited to in the root hash used as the scriptPubKey)
    # output 0: A
    # output 1: B
    # output 2..n: <HTLCs>
    settle_tx = CTransaction()
    settle_tx.nVersion = 2
    settle_tx.nLockTime = source_tx.nLockTime + 1

    # Populate the transaction inputs
    source_tx.rehash()
    outpoint = COutPoint(int(source_tx.hash, 16), 0)
    settle_tx.vin = [CTxIn(outpoint=outpoint, nSequence=CSV_DELAY)]

    # Complete output which emits 0.1 BTC to each party and 0.1 to two HTLCs
    for dest_addr, amount_sat in outputs:
        # dest_addr = node.getnewaddress(address_type="bech32")
        scriptpubkey = bytes.fromhex(node.getaddressinfo(dest_addr)['scriptPubKey'])
        dest_output = CTxOut(nValue=amount_sat, scriptPubKey=scriptpubkey)
        settle_tx.vout.append(dest_output)

    return settle_tx


def spend_update_tx(tx, funding_tx, privkey, spent_state, sighash_flag=SIGHASH_ANYPREVOUTANYSCRIPT):
    # Generate taptree for update tx at state 'spend_state'
    pubkey, _ = compute_xonly_pubkey(privkey)
    update_script = get_update_tapscript(spent_state)
    settle_script = get_settle_tapscript()
    eltoo_taptree = taproot_construct(pubkey, [
        ("update", update_script), ("settle", settle_script)
    ])

    # Generate a Taproot signature hash to spend `nValue` from any previous output with any script (ignore prevout's scriptPubKey)
    sighash = TaprootSignatureHash(
        tx,
        [funding_tx.vout[0]],
        SIGHASH_SINGLE | sighash_flag,
        input_index=0,
        scriptpath=True,
        script=CScript(),
        key_ver=KEY_VERSION_ANYPREVOUT,
    )

    # Sign with internal private key
    signature = sign_schnorr(privkey, sighash) + chr(SIGHASH_SINGLE | sighash_flag).encode('latin-1')

    # Control block created from leaf version and merkle branch information and common inner pubkey and it's negative flag
    update_leaf = eltoo_taptree.leaves["update"]
    update_control_block = bytes([update_leaf.version + eltoo_taptree.negflag]) + eltoo_taptree.inner_pubkey + update_leaf.merklebranch

    # Add witness to transaction
    inputs = [signature]
    witness_elements = [update_script, update_control_block]
    tx.wit.vtxinwit.append(CTxInWitness())
    tx.wit.vtxinwit[0].scriptWitness.stack = inputs + witness_elements


def spend_settle_tx(tx, update_tx, privkey, spent_state, sighash_flag=SIGHASH_ANYPREVOUT):
    # Generate taptree for update tx at state n
    pubkey, _ = compute_xonly_pubkey(privkey)
    update_script = get_update_tapscript(spent_state)
    settle_script = get_settle_tapscript()
    eltoo_taptree = taproot_construct(pubkey, [
        ("update", update_script), ("settle", settle_script)
    ])

    # Generate the Taproot Signature Hash for signing
    sighash = TaprootSignatureHash(
        tx,
        [update_tx.vout[0]],
        SIGHASH_SINGLE | sighash_flag,
        input_index=0,
        scriptpath=True,
        script=settle_script,
        key_ver=KEY_VERSION_ANYPREVOUT,
    )

    # Sign with internal private key
    signature = sign_schnorr(privkey, sighash) + chr(SIGHASH_SINGLE | sighash_flag).encode('latin-1')

    # Control block created from leaf version and merkle branch information and common inner pubkey and it's negative flag
    settle_leaf = eltoo_taptree.leaves["settle"]
    settle_control_block = bytes([settle_leaf.version + eltoo_taptree.negflag]) + eltoo_taptree.inner_pubkey + settle_leaf.merklebranch

    # Add witness to transaction
    inputs = [signature]
    witness_elements = [settle_script, settle_control_block]
    tx.wit.vtxinwit.append(CTxInWitness())
    tx.wit.vtxinwit[0].scriptWitness.stack = inputs + witness_elements


# eltoo taproot scripts
# see https://lists.linuxfoundation.org/pipermail/lightning-dev/2019-May/001996.html
def get_update_tapscript(state):
    # eltoo Update output
    return CScript([
        CScriptNum(CLTV_START_TIME + state),    # check state before signature
        OP_CHECKLOCKTIMEVERIFY,                 # does nothing if nLockTime of tx is a later state
        OP_DROP,                                # remove state value from stack
        OP_1,                                   # single byte 0x1 means the BIP-118 public key == taproot internal key
        OP_CHECKSIG
    ])


def get_settle_tapscript():
    # eltoo Settle output
    return CScript([
        CScriptNum(CSV_DELAY),                  # check csv delay before signature
        OP_CHECKSEQUENCEVERIFY,                 # does nothing if nSequence of tx is later than (blocks) delay
        OP_DROP,                                # remove delay value from stack
        OP_1,                                   # single byte 0x1 means the BIP-118 public key == taproot internal key
        OP_CHECKSIG
    ])


def get_htlc_claim_tapscript(preimage_hash, pubkey):
    # HTLC Claim output (with preimage)
    return CScript([
        OP_HASH160,                             # check preimage before signature
        preimage_hash,
        OP_EQUALVERIFY,
        pubkey,                                 # pubkey of party claiming payment
        OP_CHECKSIG
    ])


def get_htlc_refund_tapscript(expiry, pubkey):
    # HTLC Refund output (after expiry)
    return CScript([
        CScriptNum(expiry),                     # check htlc expiry before signature
        OP_CHECKLOCKTIMEVERIFY,                 # does not change stack if nLockTime of tx is a later time
        OP_DROP,                                # remove expiry value from stack
        pubkey,                                 # pubkey of party claiming refund
        OP_CHECKSIG
    ])


def get_eltoo_update_script(state, witness, other_witness):
    """Get the script associated with a P2PKH."""
    # or(1@and(older(100),thresh(2,pk(C),pk(C))),
    # 9@and(after(1000),thresh(2,pk(C),pk(C)))),
    return CScript([
        OP_2, witness.update_pk, other_witness.update_pk, OP_2, OP_CHECKMULTISIG,
        OP_NOTIF,
            OP_2, witness.settle_pk, other_witness.settle_pk, OP_2, OP_CHECKMULTISIGVERIFY,
            CScriptNum(CSV_DELAY), OP_CHECKSEQUENCEVERIFY,
        OP_ELSE,
            CScriptNum(CLTV_START_TIME + state), OP_CHECKLOCKTIMEVERIFY,
        OP_ENDIF,
    ])


def get_eltoo_update_script_witness(witness_program, is_update, witness, other_witness):
    script_witness = CScriptWitness()
    if is_update:
        sig1 = witness.update_sig
        sig2 = other_witness.update_sig
        script_witness.stack = [b'', sig1, sig2, witness_program]
    else:
        sig1 = witness.settle_sig
        sig2 = other_witness.settle_sig
        script_witness.stack = [b'', sig1, sig2, b'', b'', b'', witness_program]
    return script_witness


def get_eltoo_htlc_script(refund_pubkey, payment_pubkey, preimage_hash, expiry):
    return CScript([
        OP_IF,
            OP_HASH160, preimage_hash, OP_EQUALVERIFY,
            payment_pubkey,
        OP_ELSE,
            CScriptNum(expiry), OP_CHECKLOCKTIMEVERIFY, OP_DROP,
            refund_pubkey,
        OP_ENDIF,
        OP_CHECKSIG,
    ])


def get_eltoo_htlc_script_witness(witness_program, preimage, sig):
    script_witness = CScriptWitness()

    # minimal IF requires empty vector or exactly '0x01' value to prevent maleability
    if preimage is not None:
        script_witness.stack = [sig, preimage, int_to_bytes(1), witness_program]
    else:
        script_witness.stack = [sig, b'', witness_program]
    return script_witness


def get_p2pkh_script(pubkey):
    """Get the script associated with a P2PKH."""
    return CScript([OP_DUP, OP_HASH160, hash160(pubkey), OP_EQUALVERIFY, OP_CHECKSIG])


class Invoice:
    __slots__ = ("id", "preimage_hash", "amount", "expiry")

    def __init__(self, id, preimage_hash, amount, expiry):
        self.id = id
        self.preimage_hash = preimage_hash
        self.amount = amount
        self.expiry = expiry

    def deserialize(self, f):
        pass

    def serialize(self):
        pass

    def __repr__(self):
        return "Invoice(id=%i hash=%064x amount=%i expiry=%i)" % (self.id, self.preimage_hash, self.amount, self.expiry)


class Witness:
    __slots__ = "update_pk", "update_sig", "settle_pk", "settle_sig", "payment_pk"

    def __init__(self):
        self.update_pk = None
        self.update_sig = None
        self.settle_pk = None
        self.settle_sig = None
        self.payment_pk = None

    def __eq__(self, other):
        match = True
        match &= self.update_pk == other.update_pk
        match &= self.settle_pk == other.settle_pk
        match &= self.payment_pk == other.payment_pk
        # do not compare signatures, only public keys
        return match

    def set_pk(self, keys):
        self.update_pk = keys.update_key.get_pubkey().get_bytes()
        self.settle_pk = keys.settle_key.get_pubkey().get_bytes()
        self.payment_pk = keys.payment_key.get_pubkey().get_bytes()

    def __repr__(self):
        return "Witness(update_pk=%064x settle_pk=%064x payment_pk=%064x)" % (
            self.update_pk, self.settle_pk, self.payment_pk
        )


class Keys:
    __slots__ = "update_key", "settle_key", "payment_key"

    def __init__(self):
        self.update_key = ECKey()
        self.settle_key = ECKey()
        self.payment_key = ECKey()
        self.update_key.generate()
        self.settle_key.generate()
        self.payment_key.generate()


class PaymentChannel:
    __slots__ = "state", "witness", "other_witness", "spending_tx", "refund_pk", "payment_pk", "settled_refund_amount", "settled_payment_amount", "received_payments", "offered_payments"

    def __init__(self, witness, other_witness):
        self.state = 0
        self.witness = copy.copy(witness)
        self.other_witness = copy.copy(other_witness)
        self.spending_tx = None
        self.settled_refund_amount = CHANNEL_AMOUNT
        self.settled_payment_amount = 0
        self.offered_payments = {}
        self.received_payments = {}

    def total_offered_payments(self):
        total = 0
        for key, value in self.offered_payments.items():
            total += value.amount
        return total

    def total_received_payments(self):
        total = 0
        for key, value in self.received_payments.items():
            total += value.amount
        return total

    def __repr__(self):
        return "PaymentChannel(spending_tx=%064x settled_refund_amount=%i settled_payment_amount=%i offered_payments=%i received_payments=%i)" % \
               (self.spending_tx, self.settled_refund_amount, self.settled_payment_amount, self.total_offered_payments(),
                self.total_received_payments())


class UpdateTx(CTransaction):
    __slots__ = ("state", "witness", "other_witness")

    def __init__(self, payment_channel):
        super().__init__(tx=None)

        # keep a copy of initialization parameters
        self.state = payment_channel.state
        self.witness = copy.copy(payment_channel.witness)
        self.other_witness = copy.copy(payment_channel.other_witness)

        # set tx version 2 for BIP-68 outputs with relative timelocks
        self.nVersion = 2

        # initialize channel state
        self.nLockTime = CLTV_START_TIME + self.state

        # build witness program
        witness_program = get_eltoo_update_script(self.state, self.witness, self.other_witness)
        witness_hash = sha256(witness_program)
        script_wsh = CScript([OP_0, witness_hash])

        # add channel output
        self.vout = [CTxOut(CHANNEL_AMOUNT, script_wsh)]  # channel balance

    def sign(self, keys):

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append(CTxIn(outpoint=COutPoint(prevscript, 0), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE))

        tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, SIGHASH_ANYPREVOUT | SIGHASH_SINGLE, CHANNEL_AMOUNT)
        signature = keys.update_key.sign_ecdsa(tx_hash) + chr(SIGHASH_ANYPREVOUT | SIGHASH_SINGLE).encode('latin-1')

        # remove dummy vin
        self.vin.pop()

        return signature

    def verify(self):
        verified = True
        witnesses = [self.witness, self.other_witness]

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append(CTxIn(outpoint=COutPoint(prevscript, 0), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE))

        for witness in witnesses:
            pk = ECPubKey()
            pk.set(witness.update_pk)
            sig = witness.update_sig[0:-1]
            sighash = witness.update_sig[-1]
            assert sighash == (SIGHASH_ANYPREVOUT | SIGHASH_SINGLE)
            tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa(sig, tx_hash)
            if v is False:
                verified = False

        # remove dummy vin
        self.vin.pop()

        return verified

    def add_witness(self, spend_tx):
        # witness script to spend update tx to update tx
        self.wit.vtxinwit = [CTxInWitness()]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.witness.update_sig
        sig2 = self.other_witness.update_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, witness_program]
        assert len(self.vin) == 0
        self.vin = [CTxIn(outpoint=COutPoint(spend_tx.sha256, 0), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE)]


class SettleTx(CTransaction):
    __slots__ = "payment_channel"

    def __init__(self, payment_channel):
        super().__init__(tx=None)

        self.payment_channel = copy.deepcopy(payment_channel)

        # set tx version 2 for BIP-68 outputs with relative timelocks
        self.nVersion = 2

        # initialize channel state
        self.nLockTime = CLTV_START_TIME + self.payment_channel.state

        # build witness program
        witness_program = get_eltoo_update_script(self.payment_channel.state, self.payment_channel.witness,
                                                  self.payment_channel.other_witness)
        witness_hash = sha256(witness_program)
        script_wsh = CScript([OP_0, witness_hash])

        assert self.payment_channel.settled_refund_amount + self.payment_channel.settled_payment_amount + self.payment_channel.total_offered_payments() - CHANNEL_AMOUNT == 0
        settled_amounts = [self.payment_channel.settled_refund_amount, self.payment_channel.settled_payment_amount]
        signers = [self.payment_channel.witness.payment_pk, self.payment_channel.other_witness.payment_pk]
        signer_index = 0
        outputs = []
        for amount in settled_amounts:
            if amount > DUST_LIMIT:
                # pay to new p2pkh outputs, TODO: should use p2wpkh
                payment_pk = signers[signer_index]
                script_pkh = CScript([OP_0, hash160(payment_pk)])
                # self.log.debug("add_settle_outputs: state=%s, signer_index=%d, witness hash160(%s)\n", state, signer_index, ToHex(settlement_pubkey))
                outputs.append(CTxOut(amount, script_pkh))
            signer_index += 1

        for htlc_hash, htlc in self.payment_channel.offered_payments.items():
            if htlc.amount > DUST_LIMIT:
                # refund and pay to p2pkh outputs, TODO: should use p2wpkh
                refund_pubkey = self.payment_channel.witness.payment_pk
                payment_pubkey = self.payment_channel.other_witness.payment_pk
                preimage_hash = self.payment_channel.offered_payments[htlc_hash].preimage_hash
                expiry = self.payment_channel.offered_payments[htlc_hash].expiry

                # build witness program
                witness_program = get_eltoo_htlc_script(refund_pubkey, payment_pubkey, preimage_hash, expiry)
                witness_hash = sha256(witness_program)
                script_wsh = CScript([OP_0, witness_hash])
                # self.log.debug("add_settle_outputs: state=%s, signer_index=%d\n\twitness sha256(%s)=%s\n\twsh sha256(%s)=%s\n", state, signer_index, ToHex(witness_program),
                # ToHex(witness_hash), ToHex(script_wsh), ToHex(sha256(script_wsh)))
                outputs.append(CTxOut(htlc.amount, script_wsh))

        #   add settlement outputs to settlement transaction
        self.vout = outputs

    def sign(self, keys):
        # TODO: spending from a SetupTx (first UpdateTx) should not use the NOINPUT sighash 

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append(CTxIn(outpoint=COutPoint(prevscript, 0), scriptSig=b"", nSequence=CSV_DELAY))

        tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, SIGHASH_ANYPREVOUT | SIGHASH_SINGLE, CHANNEL_AMOUNT)

        signature = keys.settle_key.sign_ecdsa(tx_hash) + chr(SIGHASH_ANYPREVOUT | SIGHASH_SINGLE).encode('latin-1')

        # remove dummy vin
        self.vin.pop()

        return signature

    def verify(self):
        verified = True
        witnesses = [self.payment_channel.witness, self.payment_channel.other_witness]

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append(CTxIn(outpoint=COutPoint(prevscript, 0), scriptSig=b"", nSequence=CSV_DELAY))

        for witness in witnesses:
            pk = ECPubKey()
            pk.set(witness.settle_pk)
            sig = witness.settle_sig[0:-1]
            sighash = witness.settle_sig[-1]
            assert sighash == (SIGHASH_ANYPREVOUT | SIGHASH_SINGLE)
            tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa(sig, tx_hash)
            verified = verified and pk.verify_ecdsa(sig, tx_hash)

        # remove dummy vin
        self.vin.pop()

        return verified

    def add_witness(self, spend_tx):
        # witness script to spend update tx to settle tx
        assert spend_tx.state == self.payment_channel.state
        self.wit.vtxinwit = [CTxInWitness()]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.payment_channel.witness.settle_sig
        sig2 = self.payment_channel.other_witness.settle_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, b'', b'', b'', witness_program]
        assert len(self.vin) == 0
        self.vin = [CTxIn(outpoint=COutPoint(spend_tx.sha256, 0), scriptSig=b"", nSequence=CSV_DELAY)]


class RedeemTx(CTransaction):
    __slots__ = ("payment_channel", "secrets", "is_funder", "settled_only", "include_invalid", "block_time")

    def __init__(self, payment_channel, secrets, is_funder, settled_only, include_invalid, block_time):
        super().__init__(tx=None)

        self.payment_channel = copy.deepcopy(payment_channel)
        self.secrets = secrets
        self.is_funder = is_funder
        self.settled_only = settled_only
        self.include_invalid = include_invalid
        self.block_time = block_time

        # add settled amount (refund or payment)
        settled_amount = 0
        if self.is_funder:
            amount = self.payment_channel.settled_refund_amount
        else:
            amount = self.payment_channel.settled_payment_amount

        if amount > DUST_LIMIT:
            settled_amount = amount

        # add htlc amounts that are greater than dust and timeout has expired
        if not settled_only:
            for htlc_hash, htlc in self.payment_channel.offered_payments.items():
                if not self.include_invalid and self.is_funder and htlc.expiry > self.block_time:
                    continue
                if not self.include_invalid and not self.is_funder and htlc.preimage_hash not in self.secrets:
                    continue
                if htlc.amount > DUST_LIMIT:
                    settled_amount += htlc.amount

        # remove transaction fee from output amount
        settled_amount -= FEE_AMOUNT
        assert settled_amount > FEE_AMOUNT

        # no csv outputs, so nVersion can be 1 or 2
        self.nVersion = 2

        # refund outputs to channel funder are only spendable after a specified clock time, all others are unrestricted
        if not self.is_funder or settled_only:
            self.nLockTime = 0
        else:
            self.nLockTime = self.block_time

        #   build witness program for settled output (p2wpkh)
        pubkey = self.payment_channel.witness.payment_pk
        script_pkh = CScript([OP_0, hash160(pubkey)])

        #   add channel output
        self.vout = [CTxOut(settled_amount, script_pkh)]  # channel balance

    def sign(self, keys, htlc_index, htlc_hash):

        if htlc_hash is not None:
            # use witness program for a htlc input (p2wsh)
            assert htlc_index < len(self.payment_channel.offered_payments)
            invoice = self.payment_channel.offered_payments[htlc_hash]
            refund_pubkey = self.payment_channel.witness.payment
            payment_pubkey = self.payment_channel.other_witness.payment
            witness_program = self.get_eltoo_htlc_script(refund_pubkey, payment_pubkey, invoice.preimage_hash, invoice.expiry)
            amount = invoice.amount
        else:
            # use witness program for a settled input (p2wpkh)
            if self.is_funder is True:
                amount = self.payment_channel.settled_refund_amount
            else:
                amount = self.payment_channel.settled_payment_amount

        privkey = keys.payment_key
        tx_hash = SegwitVersion1SignatureHash(witness_program, self, input_index, SIGHASH_SINGLE, amount)
        signature = privkey.sign_ecdsa(tx_hash) + chr(SIGHASH_SINGLE).encode('latin-1')

        pk = ECPubKey()
        pk.set(privkey.get_pubkey().get_bytes())
        assert pk.verify_ecdsa(signature[0:-1], tx_hash)

        return signature

    def add_witness(self, keys, spend_tx, settled_only):
        if self.is_funder:
            signer_index = 0
        else:
            signer_index = 1
        settled_amounts = [self.payment_channel.settled_refund_amount, self.payment_channel.settled_payment_amount]

        # add settled input from htlc sender (after a timeout) or htlc receiver (with preimage)
        input_index = 0
        for amount_index in range(len(settled_amounts)):
            if settled_amounts[amount_index] > DUST_LIMIT:
                # add input from signer
                if amount_index is signer_index:
                    self.vin.append(CTxIn(outpoint=COutPoint(spend_tx.sha256, input_index), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE))
                input_index += 1

        if not settled_only:
            # add htlc inputs, one per htlc
            for htlc_hash, htlc in self.payment_channel.offered_payments.items():
                if not self.include_invalid and self.is_funder and htlc.expiry > self.block_time:
                    continue
                if not self.include_invalid and not self.is_funder and htlc.preimage_hash not in self.secrets:
                    continue
                if htlc.amount > DUST_LIMIT:
                    self.vin.append(CTxIn(outpoint=COutPoint(spend_tx.sha256, input_index), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE))
                    input_index += 1

        self.wit.vtxinwit = []
        # add the p2wpkh witness scripts to spend the settled channel amounts
        input_index = 0
        for amount_index in range(len(settled_amounts)):
            if settled_amounts[amount_index] > DUST_LIMIT:
                # add input witness from signer
                if amount_index is signer_index:
                    privkey = keys.payment_key
                    pubkey = keys.payment_key.get_pubkey().get_bytes()
                    witness_program = get_p2pkh_script(pubkey=pubkey)
                    amount = settled_amounts[amount_index]
                    # sig = self.Sign(keys=keys, htlc_index=-1, input_index=input_index)
                    tx_hash = SegwitVersion1SignatureHash(witness_program, self, input_index, SIGHASH_SINGLE, amount)
                    sig = privkey.sign_ecdsa(tx_hash) + chr(SIGHASH_SINGLE).encode('latin-1')
                    self.wit.vtxinwit.append(CTxInWitness())
                    self.wit.vtxinwit[-1].scriptWitness = CScriptWitness()
                    self.wit.vtxinwit[-1].scriptWitness.stack = [sig, pubkey]
                    input_index += 1

        if not settled_only:
            # add the p2wsh witness scripts to spend the settled channel amounts
            for htlc_hash, htlc in self.payment_channel.offered_payments.items():
                if not self.include_invalid and self.is_funder and htlc.expiry > self.block_time:
                    continue
                if not self.include_invalid and not self.is_funder and htlc.preimage_hash not in self.secrets:
                    continue
                if htlc.amount > DUST_LIMIT:
                    # generate signature for current state
                    privkey = keys.payment_key
                    refund_pubkey = self.payment_channel.witness.payment_pk
                    payment_pubkey = self.payment_channel.other_witness.payment_pk
                    witness_program = get_eltoo_htlc_script(refund_pubkey, payment_pubkey, htlc.preimage_hash, htlc.expiry)
                    amount = htlc.amount
                    # sig = self.Sign(keys=keys, htlc_index=htlc_index, input_index=input_index)
                    tx_hash = SegwitVersion1SignatureHash(witness_program, self, input_index, SIGHASH_SINGLE, amount)
                    sig = privkey.sign_ecdsa(tx_hash) + chr(SIGHASH_SINGLE).encode('latin-1')
                    self.wit.vtxinwit.append(CTxInWitness())
                    if self.is_funder:
                        preimage = None
                    else:
                        preimage = self.secrets[htlc.preimage_hash]
                    self.wit.vtxinwit[-1].scriptWitness = get_eltoo_htlc_script_witness(witness_program, preimage, sig)
                    witness_hash = sha256(witness_program)
                    script_wsh = CScript([OP_0, witness_hash])
                    input_index += 1


class CloseTx(CTransaction):
    __slots__ = ("payment_channel", "setup_tx")

    def __init__(self, payment_channel, setup_tx):
        super().__init__(tx=None)

        self.payment_channel = copy.deepcopy(payment_channel)
        self.setup_tx = setup_tx

        for htlc_hash, htlc in self.payment_channel.offered_payments.items():
            # assume payer sweeps all unfulfilled HTLCs
            self.payment_channel.refund_amount += htlc.amount

        # sanity check
        assert self.payment_channel.settled_refund_amount + self.payment_channel.settled_payment_amount == CHANNEL_AMOUNT
        self.payment_channel.offered_payments.clear()

        # remove transaction fee from output amounts
        if self.payment_channel.settled_refund_amount > self.payment_channel.settled_payment_amount:
            self.payment_channel.settled_payment_amount -= min(int(FEE_AMOUNT / 2), self.payment_channel.settled_payment_amount)
            self.payment_channel.settled_refund_amount = CHANNEL_AMOUNT - self.payment_channel.settled_payment_amount - FEE_AMOUNT
        else:
            self.payment_channel.settled_refund_amount -= min(int(FEE_AMOUNT / 2), self.payment_channel.settled_refund_amount)
            self.payment_channel.settled_payment_amount = CHANNEL_AMOUNT - self.payment_channel.settled_refund_amount - FEE_AMOUNT
        assert self.payment_channel.settled_refund_amount + self.payment_channel.settled_payment_amount + FEE_AMOUNT == CHANNEL_AMOUNT

        # no csv outputs, so nVersion can be 1 or 2
        self.nVersion = 2

        # refund outputs to channel partners immediately
        self.nLockTime = CLTV_START_TIME + self.payment_channel.state + 1

        # add setup_tx vin
        self.vin = [CTxIn(outpoint=COutPoint(setup_tx.sha256, 0), scriptSig=b"", nSequence=DEFAULT_NSEQUENCE)]

        # build witness program for settled refund output (p2wpkh)
        pubkey = self.payment_channel.witness.payment_pk
        script_pkh = CScript([OP_0, hash160(pubkey)])

        outputs = []

        # refund output
        if self.payment_channel.settled_refund_amount > DUST_LIMIT:
            outputs.append(CTxOut(self.payment_channel.settled_refund_amount, script_pkh))

        # build witness program for settled payment output (p2wpkh)
        pubkey = self.payment_channel.other_witness.payment_pk
        script_pkh = CScript([OP_0, hash160(pubkey)])

        # settled output
        if self.payment_channel.settled_payment_amount > DUST_LIMIT:
            outputs.append(CTxOut(self.payment_channel.settled_payment_amount, script_pkh))

        self.vout = outputs

    def is_channel_funder(self, keys):
        pubkey = keys.update_key.get_pubkey().get_bytes()
        if pubkey == self.payment_channel.witness.update_pk:
            return True
        else:
            return False

    def sign(self, keys, setup_tx):

        # spending from a SetupTx (first UpdateTx) should not use the NOINPUT sighash 
        witness_program = get_eltoo_update_script(setup_tx.state, setup_tx.witness, setup_tx.other_witness)
        tx_hash = SegwitVersion1SignatureHash(witness_program, self, 0, SIGHASH_SINGLE, CHANNEL_AMOUNT)
        signature = keys.update_key.sign_ecdsa(tx_hash) + chr(SIGHASH_SINGLE).encode('latin-1')

        if self.is_channel_funder(keys):
            self.payment_channel.witness.update_sig = signature
        else:
            self.payment_channel.other_witness.update_sig = signature

        return signature

    def verify(self, setup_tx):
        verified = True
        witnesses = [self.payment_channel.witness, self.payment_channel.other_witness]

        for witness in witnesses:
            pk = ECPubKey()
            pk.set(witness.update_pk)
            sig = witness.update_sig[0:-1]
            sighash = witness.update_sig[-1]
            assert sighash == SIGHASH_SINGLE
            witness_program = get_eltoo_update_script(setup_tx.state, setup_tx.witness, setup_tx.other_witness)
            tx_hash = SegwitVersion1SignatureHash(witness_program, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa(sig, tx_hash)
            verified = verified and pk.verify_ecdsa(sig, tx_hash)

        return verified

    def add_witness(self, spend_tx):
        # witness script to spend update tx to close tx
        self.wit.vtxinwit = [CTxInWitness()]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.payment_channel.witness.update_sig
        sig2 = self.payment_channel.other_witness.update_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, witness_program]


class L2Node:
    __slots__ = "gid", "issued_invoices", "secrets", "payment_channels", "keychain", "complete_payment_channels"

    def __init__(self, gid):
        self.gid = gid
        self.issued_invoices = []
        self.secrets = {}
        self.payment_channels = {}
        self.keychain = {}
        self.complete_payment_channels = {}

    def __hash__(self):
        return hash(self.gid)

    def __eq__(self, other):
        return self.gid == other.gid

    def __ne__(self, other):
        # Not strictly necessary, but to avoid having both x==y and x!=y
        # True at the same time
        return not (self == other)

    def is_channel_funder(self, channel_partner):
        pubkey = self.keychain[channel_partner].update_key.get_pubkey().get_bytes()
        if pubkey == self.payment_channels[channel_partner].witness.update_pk:
            return True
        else:
            return False

    def propose_channel(self):
        keys = Keys()
        witness = Witness()
        witness.set_pk(keys)

        return keys, witness

    def JoinChannel(self, channel_partner, witness):
        # generate local keys for proposed channel
        self.keychain[channel_partner] = Keys()
        other_witness = Witness()
        other_witness.set_pk(self.keychain[channel_partner])

        # initialize a new payment channel
        self.payment_channels[channel_partner] = PaymentChannel(witness, other_witness)

        # no need to sign an Update Tx transaction, only need to sign a SettleTx that refunds the first Setup Tx

        # sign settle transaction
        assert self.payment_channels[channel_partner].state == 0
        settle_tx = SettleTx(payment_channel=self.payment_channels[channel_partner])
        self.payment_channels[channel_partner].other_witness.settle_sig = settle_tx.sign(self.keychain[channel_partner])

        # create the first Update Tx (aka Setup Tx)
        self.payment_channels[channel_partner].spending_tx = UpdateTx(self.payment_channels[channel_partner])

        # return witness updated with valid signatures for the refund settle transaction
        return self.payment_channels[channel_partner].other_witness

    def create_channel(self, channel_partner, keys, witness, other_witness):
        # use keys created for this payment channel by ProposeChannel
        self.keychain[channel_partner] = keys

        # initialize a new payment channel
        self.payment_channels[channel_partner] = PaymentChannel(witness, other_witness)
        assert len(self.keychain) == len(self.payment_channels)

        # no need to sign an Update Tx transaction, only need to sign a SettleTx that refunds the first Setup Tx

        # sign settle transaction
        assert self.payment_channels[channel_partner].state == 0
        settle_tx = SettleTx(payment_channel=self.payment_channels[channel_partner])
        signature = settle_tx.sign(keys)

        # save signature to payment channel and settle tx
        self.payment_channels[channel_partner].witness.settle_sig = signature
        settle_tx.payment_channel.witness.settle_sig = signature

        # check that we can create a valid refund/settle transaction to use if we need to close the channel
        assert settle_tx.verify()

        # create the first Update Tx (aka Setup Tx)
        setup_tx = UpdateTx(self.payment_channels[channel_partner])

        # save the most recent co-signed payment_channel state that can be used to uncooperatively close the channel
        self.complete_payment_channels[channel_partner] = (copy.deepcopy(self.payment_channels[channel_partner]), copy.deepcopy(self.keychain[channel_partner]))

        return setup_tx, settle_tx

    def create_invoice(self, id, amount, expiry):
        secret = random.randrange(1, SECP256K1_ORDER).to_bytes(32, 'big')
        self.secrets[hash160(secret)] = secret
        invoice = Invoice(id=id, preimage_hash=hash160(secret), amount=amount, expiry=expiry)
        return invoice

    def learn_secret(self, secret):
        self.secrets[hash160(secret)] = secret

    def propose_payment(self, channel_partner, invoice, prev_update_tx):

        # generate new keys for the settle transaction unique to the next state
        keys = self.keychain[channel_partner]
        keys.settle_key = ECKey()
        keys.settle_key.generate()

        # assume we have xpub from channel partner we can use to generate a new settle pubkey for them
        other_settle_key = ECKey()
        other_settle_key.generate()

        # create updated payment channel information for the next proposed payment channel state
        payment_channel = self.payment_channels[channel_partner]
        payment_channel.state += 1
        payment_channel.witness.settle_pk = keys.settle_key.get_pubkey().get_bytes()
        payment_channel.other_witness.settle_pk = other_settle_key.get_pubkey().get_bytes()
        payment_channel.settled_refund_amount -= invoice.amount
        payment_channel.offered_payments[invoice.preimage_hash] = invoice

        # save updated payment channel state
        self.payment_channels[channel_partner] = payment_channel

        # save updated keys
        self.keychain[channel_partner] = keys

        # create an update tx that spends any update tx with an earlier state
        update_tx = UpdateTx(payment_channel)

        # sign with new update key
        update_sig = update_tx.sign(self.keychain[channel_partner])
        update_tx.witness.update_sig = update_sig

        # create a settle tx that spends the new update tx
        settle_tx = SettleTx(payment_channel)

        # sign with new update key for this state
        settle_sig = settle_tx.sign(self.keychain[channel_partner])
        settle_tx.payment_channel.witness.settle_sig = settle_sig

        return update_tx, settle_tx, other_settle_key

    def receive_payment(self, channel_partner, update_tx, settle_tx, other_settle_key):

        # assume we generated same new settle key from xpub as channel partner
        self.keychain[channel_partner].settle_key = other_settle_key

        # check that new payment channel state passes sanity checks
        payment_channel = settle_tx.payment_channel
        assert payment_channel.witness == update_tx.witness
        assert payment_channel.other_witness == update_tx.other_witness
        assert payment_channel.state == update_tx.state
        assert payment_channel.state > self.payment_channels[channel_partner].state
        assert payment_channel.settled_refund_amount + payment_channel.settled_payment_amount + payment_channel.total_offered_payments() - CHANNEL_AMOUNT == 0
        assert payment_channel.settled_payment_amount >= self.payment_channels[channel_partner].settled_payment_amount
        assert payment_channel.total_offered_payments() > self.payment_channels[channel_partner].total_offered_payments()

        # sign update tx with my key
        update_sig = update_tx.sign(self.keychain[channel_partner])
        update_tx.other_witness.update_sig = update_sig

        # sign settle tx with new settle key
        settle_sig = settle_tx.sign(self.keychain[channel_partner])
        settle_tx.payment_channel.other_witness.settle_sig = settle_sig

        # verify both channel partners signed the txs
        assert update_tx.verify()
        assert settle_tx.verify()

        # accept new channel state
        self.payment_channels[channel_partner] = payment_channel

        # check if we know the secret
        found_htlc = None
        secret = None
        for hashed_secret, tmp_secret in self.secrets.items():
            found_htlc = payment_channel.offered_payments.get(hashed_secret, None)
            if found_htlc is not None:
                secret = tmp_secret
                break

        return update_tx, settle_tx, secret

    def uncooperatively_close(self, channel_partner, settle_tx, settled_only=False, include_invalid=True, block_time=0):

        # create an redeem tx that spends a committed settle tx
        is_funder = self.is_channel_funder(channel_partner)
        redeem_tx = RedeemTx(payment_channel=settle_tx.payment_channel, secrets=self.secrets, is_funder=is_funder, settled_only=settled_only, include_invalid=include_invalid, block_time=block_time)
        redeem_tx.add_witness(keys=self.keychain[channel_partner], spend_tx=settle_tx, settled_only=settled_only)

        return redeem_tx

    def propose_update(self, channel_partner, block_time):
        # payment receiver creates new update tx and settle tx with new settled balances based on invoices that can be redeemed or expired
        assert not self.is_channel_funder(channel_partner)

        # most recent co-signed payment_channel state that can be used to uncooperatively close the channel
        self.complete_payment_channels[channel_partner] = (copy.deepcopy(self.payment_channels[channel_partner]), copy.deepcopy(self.keychain[channel_partner]))

        redeemed_secrets = {}
        removed_invoices = []
        for htlc_hash, invoice in self.payment_channels[channel_partner].offered_payments.items():
            if invoice.expiry < block_time:
                # remove expired invoices, credit back as settled refund
                removed_invoices.append(htlc_hash)
                self.payment_channels[channel_partner].settled_refund_amount += invoice.amount
            elif self.secrets.get(htlc_hash, None) is not None:
                # remove redeemed invoices, credit as settled payments
                removed_invoices.append(htlc_hash)
                redeemed_secrets[htlc_hash] = self.secrets.get(htlc_hash)
                self.payment_channels[channel_partner].settled_payment_amount += invoice.amount

        for htlc_hash in removed_invoices:
            self.payment_channels[channel_partner].offered_payments.pop(htlc_hash, None)

        # generate new keys for the settle transaction unique to the next state, do not replace
        self.keychain[channel_partner].settle_key = ECKey()
        self.keychain[channel_partner].settle_key.generate()

        # assume we have xpub from channel partner we can use to generate a new settle pubkey for them
        settle_key = ECKey()
        settle_key.generate()

        # create updated payment channel information for the next proposed payment channel state
        self.payment_channels[channel_partner].state += 1
        self.payment_channels[channel_partner].witness.settle_pk = settle_key.get_pubkey().get_bytes()
        self.payment_channels[channel_partner].other_witness.settle_pk = self.keychain[channel_partner].settle_key.get_pubkey().get_bytes()

        # create an update tx that spends any update tx with an earlier state
        update_tx = UpdateTx(self.payment_channels[channel_partner])

        # sign with new update keys
        update_sig = update_tx.sign(self.keychain[channel_partner])
        update_tx.other_witness.update_sig = update_sig

        # create a settle tx that spends the new update tx
        settle_tx = SettleTx(self.payment_channels[channel_partner])

        # sign with new pending settle key for this state
        settle_sig = settle_tx.sign(self.keychain[channel_partner])
        settle_tx.payment_channel.other_witness.settle_sig = settle_sig

        return update_tx, settle_tx, settle_key, redeemed_secrets

    def accept_update(self, channel_partner, update_tx, settle_tx, settle_key, redeemed_secrets, block_time):
        # payment sender confirms the new update tx and settle tx with new settled balances based on invoices that can be redeemed or expired
        assert self.is_channel_funder(channel_partner)

        updated_payment_channel = copy.deepcopy(self.payment_channels[channel_partner])
        updated_payment_channel.state += 1
        removed_invoices = []
        for htlc_hash, invoice in updated_payment_channel.offered_payments.items():
            if invoice.expiry < block_time:
                # remove expired invoices, credit back as settled refund
                removed_invoices.append(htlc_hash)
                updated_payment_channel.settled_refund_amount += invoice.amount
            elif redeemed_secrets.get(htlc_hash, None) is not None:
                removed_invoices.append(htlc_hash)
                updated_payment_channel.settled_payment_amount += invoice.amount

        for htlc_hash in removed_invoices:
            updated_payment_channel.offered_payments.pop(htlc_hash, None)

        is_valid = updated_payment_channel.settled_payment_amount == settle_tx.payment_channel.settled_payment_amount
        is_valid &= updated_payment_channel.settled_refund_amount == settle_tx.payment_channel.settled_refund_amount
        is_valid &= updated_payment_channel.offered_payments == settle_tx.payment_channel.offered_payments

        if is_valid:
            self.payment_channels[channel_partner] = updated_payment_channel
            self.keychain[channel_partner].settle_key = settle_key

            # learn new secrets 
            for hashed_secret, secret in redeemed_secrets.items():
                self.learn_secret(secret)

            # sign with update key
            update_sig = update_tx.sign(self.keychain[channel_partner])
            update_tx.witness.update_sig = update_sig

            # sign with new settle key for this state
            settle_sig = settle_tx.sign(self.keychain[channel_partner])
            settle_tx.payment_channel.witness.settle_sig = settle_sig
        else:
            return None, None

        return update_tx, settle_tx

    def confirm_update(self, channel_partner, update_tx, settle_tx):
        # payment receiver confirms the new update tx and settle tx with new settled balances was signed by payment sender
        assert not self.is_channel_funder(channel_partner)

        # if both channel partners signed the new update tx and settle tx, then update to the new payment_channel state
        if update_tx.verify() and settle_tx.verify():
            return True
        else:
            # should now do a noncooperative close from the last completed state signed by the payer
            self.payment_channels[channel_partner], self.keychain[channel_partner] = copy.deepcopy(self.complete_payment_channels[channel_partner])
            return False

    def propose_close(self, channel_partner, setup_tx):
        # create a clise tx that spends a committed setup tx immediately with the settled balances
        close_tx = CloseTx(payment_channel=self.payment_channels[channel_partner], setup_tx=setup_tx)
        close_tx.sign(keys=self.keychain[channel_partner], setup_tx=setup_tx)
        return close_tx

    def accept_close(self, channel_partner, close_tx, setup_tx):
        # confirm settled amounts are as expected
        tmp_close_tx = CloseTx(payment_channel=self.payment_channels[channel_partner], setup_tx=setup_tx)
        is_valid = close_tx.payment_channel.settled_payment_amount == tmp_close_tx.payment_channel.settled_payment_amount
        is_valid &= close_tx.payment_channel.settled_refund_amount == tmp_close_tx.payment_channel.settled_refund_amount

        if is_valid:
            close_tx.sign(keys=self.keychain[channel_partner], setup_tx=setup_tx)
            if close_tx.verify(setup_tx):
                # add witness information to close tx so it can be committed
                close_tx.add_witness(setup_tx)
                return close_tx

        return None


class SimulateL2Tests(BitcoinTestFramework):

    def next_block(self, number, spend=None, additional_coinbase_value=0, script=CScript([OP_TRUE]), solve=True, *, version=1):
        if self.tip is None:
            base_block_hash = self.genesis_hash
            block_time = int(self.start_time) + 1
        else:
            base_block_hash = self.tip.sha256
            block_time = self.tip.nTime + 1
        # First create the coinbase
        height = self.block_heights[base_block_hash] + 1
        coinbase = create_coinbase(height, self.coinbase_pubkey)
        coinbase.vout[0].nValue += additional_coinbase_value
        coinbase.rehash()
        if spend is None:
            block = create_block(base_block_hash, coinbase, block_time, version=version)
        else:
            coinbase.vout[0].nValue += spend.vout[0].nValue - 1  # all but one satoshi to fees
            coinbase.rehash()
            block = create_block(base_block_hash, coinbase, block_time, version=version)
            tx = self.create_tx(spend, 0, 1, script)  # spend 1 satoshi
            sign_tx(tx, spend)
            self.add_transactions_to_block(block, [tx])
            block.hashMerkleRoot = block.calc_merkle_root()
        if solve:
            block.solve()
        self.tip = block
        self.block_heights[block.sha256] = height
        assert number not in self.blocks
        self.blocks[number] = block
        return block

    def send_blocks(self, blocks, success=True, reject_reason=None, force_send=False, reconnect=False, timeout=60):
        """Sends blocks to test node. Syncs and verifies that tip has advanced to most recent block.

        Call with success = False if the tip shouldn't advance to the most recent block."""
        self.helper_peer.send_blocks_and_test(blocks, self.nodes[0], success=success, reject_reason=reject_reason, force_send=force_send, timeout=timeout, expect_disconnect=reconnect)

        if reconnect:
            self.reconnect_p2p(timeout=timeout)

    # save the current tip so it can be spent by a later block
    def save_spendable_output(self):
        self.log.debug("saving spendable output %s" % self.tip.vtx[0])
        self.spendable_outputs.append(self.tip)

    # get an output that we previously marked as spendable
    def get_spendable_output(self):
        self.log.debug("getting spendable output %s" % self.spendable_outputs[0].vtx[0])
        return self.spendable_outputs.pop(0).vtx[0]

    def bootstrap_p2p(self, timeout=10):
        """Add a P2P connection to the node.

        Helper to connect and wait for version handshake."""
        self.helper_peer = self.nodes[0].add_p2p_connection(P2PDataStore())
        # We need to wait for the initial getheaders from the peer before we
        # start populating our blockstore. If we don't, then we may run ahead
        # to the next subtest before we receive the getheaders. We'd then send
        # an INV for the next block and receive two getheaders - one for the
        # IBD and one for the INV. We'd respond to both and could get
        # unexpectedly disconnected if the DoS score for that error is 50.
        self.helper_peer.wait_for_getheaders(timeout=timeout)

    def init_coinbase(self):
        self.bootstrap_p2p()  # Add one p2p connection to the node

        # TODO: use self.nodes[0].get_deterministic_priv_key and b58decode_chk instead of generating my own blocks!
        self.tip = None
        self.blocks = {}
        self.block_heights = {}
        self.spendable_outputs = []

        self.genesis_hash = int(self.nodes[0].getbestblockhash(), 16)
        self.block_heights[self.genesis_hash] = 0

        self.coinbase_key = ECKey()
        self.coinbase_key.generate()
        self.coinbase_pubkey = self.coinbase_key.get_pubkey().get_bytes()

        # set initial blocktime to current time
        self.start_time = int(1500000000)  # int(time.time())
        self.nodes[0].setmocktime = self.start_time

        # generate mature coinbase to spend
        NUM_BUFFER_BLOCKS_TO_GENERATE = 110
        for i in range(NUM_BUFFER_BLOCKS_TO_GENERATE):
            bn = self.next_block(i)
            self.save_spendable_output()
            self.send_blocks([bn])

        blockheight = self.nodes[0].getblockheader(blockhash=self.nodes[0].getbestblockhash())['height']

        # collect spendable outputs now to avoid cluttering the code later on
        self.coinbase_utxo = []
        for i in range(NUM_OUTPUTS_TO_COLLECT):
            self.coinbase_utxo.append(self.get_spendable_output())
        self.coinbase_index = 0

        self.nodes[0].generate(33)

    def fund(self, tx, spend_tx, amount):
        assert self.coinbase_index < NUM_OUTPUTS_TO_COLLECT
        assert amount >= FEE_AMOUNT

        fund_tx = self.coinbase_utxo[self.coinbase_index]
        assert fund_tx is not None
        self.coinbase_index += 1
        fund_key = self.coinbase_key
        outIdx = 0

        # update vin and witness to spend a specific update tx (skipped for setup tx)
        if spend_tx is not None:
            tx.add_witness(spend_tx)

        # pay change to new p2pkh output, TODO: should use p2wpkh
        change_key = ECKey()
        change_key.generate()
        change_pubkey = change_key.get_pubkey().get_bytes()
        change_script_pkh = CScript([OP_0, hash160(change_pubkey)])
        change_amount = fund_tx.vout[0].nValue - amount

        # add new funding input and change output
        tx.vin.append(CTxIn(COutPoint(fund_tx.sha256, 0), b""))
        tx.vout.append(CTxOut(change_amount, change_script_pkh))

        # pay fee from spend_tx w/change output (assumed to be last txin)
        inIdx = len(tx.vin) - 1

        # sign the tx fee input w/change output
        scriptPubKey = bytearray(fund_tx.vout[outIdx].scriptPubKey)
        (sighash, err) = LegacySignatureHash(fund_tx.vout[0].scriptPubKey, tx, inIdx, SIGHASH_ALL)
        sig = fund_key.sign_ecdsa(sighash) + bytes(bytearray([SIGHASH_ALL]))
        tx.vin[inIdx].scriptSig = CScript([sig])

        # update the hash of this transaction
        tx.rehash()

        return change_key, change_amount

    def commit(self, tx, error_code=None, error_message=None):
        # update hash
        tx.rehash()

        # confirm it is in the mempool
        tx_hex = ToHex(tx)
        if error_code is None or error_message is None:
            txid = self.nodes[0].sendrawtransaction(tx_hex)
        else:
            txid = assert_raises_rpc_error(error_code, error_message, self.nodes[0].sendrawtransaction, tx_hex)
        return txid

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[]]

    def test_bitcoinops_workshop_tapscript(self):

        # create schnorr privkey and x only pubkey keys
        privkey1 = generate_privkey()
        pubkey1, _ = compute_xonly_pubkey(privkey1)
        privkey2 = generate_privkey()
        pubkey2, _ = compute_xonly_pubkey(privkey2)
        print("pubkey1: {}".format(pubkey1.hex()))
        print("pubkey2: {}".format(pubkey2.hex()))

        # Method: 32B preimage - sha256(bytes)
        # Method: 20B digest - hash160(bytes)
        secret = b'secret'
        preimage = sha256(secret)
        digest = hash160(preimage)
        delay = 20

        # Construct tapscript
        csa_delay_tapscript = CScript([
            pubkey1,
            OP_CHECKSIG,
            pubkey2,
            OP_CHECKSIGADD,
            2,
            OP_NUMEQUAL,
            OP_VERIFY,
            14,
            OP_CHECKSEQUENCEVERIFY
        ])

        print("csa_delay_tapscript operations:")
        for op in csa_delay_tapscript:
            print(op.hex()) if isinstance(op, bytes) else print(op)

        privkey_internal = generate_privkey()
        pubkey_internal, _ = compute_xonly_pubkey(privkey_internal)

        # create taptree from internal public key and list of (name, script) tuples
        taptree = taproot_construct(pubkey_internal, [("csa_delay", csa_delay_tapscript)])

        # Tweak the internal key to obtain the Segwit program
        tweaked, _ = tweak_add_pubkey(pubkey_internal, taptree.tweak)

        # Create (regtest) bech32 address from 32-byte tweaked public key
        address = program_to_witness(version=0x01, program=tweaked, main=False)
        print("bech32 address is {}".format(address))
        print("Taproot witness program, len={} is {}\n".format(len(tweaked), tweaked.hex()))

        print("scriptPubKey operations:\n")
        for op in taptree.scriptPubKey:
            print(op.hex()) if isinstance(op, bytes) else print(op)

        # Generate coins and create an output
        tx = generate_and_send_coins(self.nodes[0], address, 100_000_000)
        print("Transaction {}, output 0\nsent to {}\n".format(tx.hash, address))

        # Create a spending transaction
        spending_tx = create_spending_transaction(self.nodes[0], tx.hash, version=2, nSequence=delay)
        print("Spending transaction:\n{}".format(spending_tx))

        # Generate the Taproot Signature Hash for signing
        sighash = TaprootSignatureHash(
            spending_tx,
            [tx.vout[0]],
            SIGHASH_DEFAULT,
            input_index=0,
            scriptpath=True,
            script=csa_delay_tapscript,
        )

        # Sign with both privkeys
        signature1 = sign_schnorr(privkey1, sighash)
        signature2 = sign_schnorr(privkey2, sighash)

        print("Signature1: {}".format(signature1.hex()))
        print("Signature2: {}".format(signature2.hex()))

        # Control block created from leaf version and merkle branch information and common inner pubkey and it's negative flag
        leaf = taptree.leaves["csa_delay"]
        control_block = bytes([leaf.version + taptree.negflag]) + taptree.inner_pubkey + leaf.merklebranch

        # Add witness to transaction
        inputs = [signature2, signature1]
        witness_elements = [csa_delay_tapscript, control_block]
        spending_tx.wit.vtxinwit.append(CTxInWitness())
        spending_tx.wit.vtxinwit[0].scriptWitness.stack = inputs + witness_elements

        print("Spending transaction:\n{}\n".format(spending_tx))

        # Test mempool acceptance with and without delay
        assert not test_transaction(self.nodes[0], spending_tx)
        self.nodes[0].generate(delay)
        assert test_transaction(self.nodes[0], spending_tx)

        print("Success!")

    # test eltoo tapscript tx
    def test_tapscript_eltoo(self):

        # Musig(A,B) schnorr key to use as the taproot internal key for eltoo TS outputs
        privkey_AB = generate_privkey()
        pubkey_AB, _ = compute_xonly_pubkey(privkey_AB)

        # schnorr keys to spend A balance and htlcs
        privkey_A = generate_privkey()
        pubkey_A, _ = compute_xonly_pubkey(privkey_A)

        # schnorr keys to spend B balance and htlcs
        privkey_B = generate_privkey()
        pubkey_B, _ = compute_xonly_pubkey(privkey_B)

        # htlc witness data
        secret1 = b'secret1'
        preimage1_hash = hash160(secret1)
        secret2 = b'secret2'
        preimage2_hash = hash160(secret2)
        expiry = self.start_time + INVOICE_TIMEOUT

        # addresses for settled balances
        toA_address = self.nodes[0].getnewaddress(address_type="bech32")
        toB_address = self.nodes[0].getnewaddress(address_type="bech32")

        # addresses for each eltoo state or htlc output
        state0_address = get_state_address(pubkey_AB, 0)
        state1_address = get_state_address(pubkey_AB, 1)
        htlc0_address = get_htlc_address(pubkey_AB, preimage1_hash, pubkey_A, expiry, pubkey_B)
        htlc1_address = get_htlc_address(pubkey_AB, preimage2_hash, pubkey_A, expiry, pubkey_B)

        # generate coins and create an output at state 0
        update0_tx = generate_and_send_coins(self.nodes[0], state0_address, CHANNEL_AMOUNT)

        # create and spend output at state 0 -> state 1
        update1_tx = create_update_transaction(self.nodes[0], source_tx=update0_tx, dest_addr=state1_address, state=1, amount_sat=CHANNEL_AMOUNT)
        spend_update_tx(update1_tx, update0_tx, privkey_AB, spent_state=0)

        # create and spend output at state 0 -> settlement outputs at state 0 (scriptPubKey and amount must match update0_tx) 
        settle0_tx = create_settle_transaction(self.nodes[0], source_tx=update0_tx, outputs=[(toA_address, CHANNEL_AMOUNT), (htlc0_address, 1000)])
        spend_settle_tx(settle0_tx, update0_tx, privkey_AB, spent_state=0)

        # create and spend output at state 0 -> settlement outputs at state 1 (scriptPubKey and amount must match update1_tx)
        settle1_apo_tx = create_settle_transaction(self.nodes[0], source_tx=update0_tx, outputs=[(toA_address, CHANNEL_AMOUNT - 2000), (toB_address, 1000), (htlc1_address, 1000)])
        spend_settle_tx(settle1_apo_tx, update1_tx, privkey_AB, spent_state=0, sighash_flag=SIGHASH_ANYPREVOUT)

        # create and spend output at state 0 -> settlement outputs at state 1 that (only amount must match update1_tx)
        settle1_apoas_tx = create_settle_transaction(self.nodes[0], source_tx=update0_tx, outputs=[(toA_address, CHANNEL_AMOUNT - 2000), (toB_address, 1000), (htlc1_address, 1000)])
        spend_settle_tx(settle1_apoas_tx, update1_tx, privkey_AB, spent_state=0, sighash_flag=SIGHASH_ANYPREVOUTANYSCRIPT)

        # add inputs for transaction fees
        self.fund(tx=update1_tx, spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        self.fund(tx=settle0_tx, spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        self.fund(tx=settle1_apo_tx, spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        self.fund(tx=settle1_apoas_tx, spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)

        # ---------------------------------------------------------------------

        # succeed: update0_tx -> update1_tx
        assert test_transaction(self.nodes[0], update1_tx)

        # fail: settle update0_tx -> settle0_tx (before CSV delay)
        assert not test_transaction(self.nodes[0], settle0_tx)
        self.nodes[0].generate(CSV_DELAY)

        # succeed: settle update0_tx -> settle0_tx (after CSV delay)
        assert test_transaction(self.nodes[0], settle0_tx)

        # fail: update0_tx -> settle1_apo_tx (SIGHASH_ANYPREVOUT commits to scriptPubKey of update1_tx so signature is invalid to spend from update0_tx output)
        assert not test_transaction(self.nodes[0], settle1_apo_tx)

        # succeed: settle update0_tx -> settle1_apoas_tx (SIGHASH_ANYPREVOUTANYSCRIPT does not commit to scriptPubKey so signature is valid to spend from update0_tx output)
        assert test_transaction(self.nodes[0], settle1_apoas_tx)

        print("Success!")

    def test1(self, payer_sweeps_utxo):
        # topology: node1 opens a channel with node 2
        A = L2Node(1)
        B = L2Node(2)

        # create the set of keys needed to open a new payment channel
        keys, witness = A.propose_channel()

        # create a new channel and sign the initial refund of a new payment channel
        other_witness = B.JoinChannel(channel_partner=A, witness=witness)

        # create a new channel
        setup_tx, refund_tx = A.create_channel(
            channel_partner=B,
            keys=keys,
            witness=witness,
            other_witness=other_witness
        )

        # fund and commit the setup tx to create the new channel
        self.fund(tx=setup_tx, spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        txid = self.commit(setup_tx)

        # mine the setup tx into a new block
        self.nodes[0].setmocktime = self.start_time
        self.nodes[0].generate(1)

        # A tries to commit the refund tx before the CSV delay has expired
        self.fund(tx=refund_tx, spend_tx=setup_tx, amount=FEE_AMOUNT)
        txid = self.commit(refund_tx, error_code=-26, error_message="non-BIP68-final")

        # B creates first invoice
        invoice = B.create_invoice('test1a', amount=10000, expiry=self.start_time + INVOICE_TIMEOUT)

        # A creates partially signed transactions to pay the invoice from B
        (update1_tx, settle1_tx, other_settle1_key) = A.propose_payment(B, invoice, setup_tx)

        # B receives payment from A and returns corresponding secret and fully signed transactions
        (update1_tx, settle1_tx, secret) = B.receive_payment(A, update1_tx, settle1_tx, other_settle1_key)

        # mine the setup tx into a new block, increment time by 10 minutes
        self.nodes[0].generate(1)

        # B creates second invoice
        invoice = B.create_invoice('test1b', amount=10000, expiry=self.start_time + INVOICE_TIMEOUT + BLOCK_TIME)

        # A creates a new partially signed transactions with higher state to pay the second invoice from B
        (update2_tx, settle2_tx, other_settle2_key) = A.propose_payment(B, invoice, setup_tx)

        # B receives payment from A and returns corresponding secret and fully signed transactions
        (update2_tx, settle2_tx, secret) = B.receive_payment(A, update2_tx, settle2_tx, other_settle2_key)

        # fund the transaction fees for the update tx before committing it
        self.fund(tx=update2_tx, spend_tx=setup_tx, amount=FEE_AMOUNT)

        # B commits the update tx to start the uncooperative close of the channel
        txid = self.commit(update2_tx)

        # fund the transaction fees for the settle tx before committing it
        self.fund(tx=settle2_tx, spend_tx=update2_tx, amount=FEE_AMOUNT)

        # B tries to commits the settle tx before the CSV timeout
        txid = self.commit(settle2_tx, error_code=-26, error_message="non-BIP68-final")

        # A tries to commit an update tx that spends the committed update to an earlier state (encoded by nLocktime)
        self.fund(tx=update1_tx, spend_tx=update2_tx, amount=FEE_AMOUNT)
        txid = self.commit(update1_tx, error_code=-26, error_message="non-mandatory-script-verify-flag")  # Locktime requirement not satisfied

        # mine the update tx into a new block, and advance blocks until settle tx can be spent
        self.nodes[0].generate(CSV_DELAY + 1)

        # A or B commits the settle tx to finalize the uncooperative close of the channel after the CSV timeout
        txid = self.commit(settle2_tx)

        if payer_sweeps_utxo:
            # A collects the settled amount and tries to sweep even invalid/unexpired htlcs
            redeem_tx = A.uncooperatively_close(B, settle2_tx, include_invalid=True, block_time=self.start_time + INVOICE_TIMEOUT)

            # wait for invoice to expire
            self.nodes[0].setmocktime = (self.start_time + INVOICE_TIMEOUT)
            self.nodes[0].generate(1)

            # A attempts to commits the redeem tx to complete the uncooperative close of the channel before the invoice has timed out
            txid = self.commit(redeem_tx, error_code=-26, error_message="non-mandatory-script-verify-flag")  # (Locktime requirement not satisfied)

            # advance locktime on redeem tx to past expiry of the invoices
            redeem_tx = A.uncooperatively_close(B, settle2_tx, include_invalid=False, block_time=self.start_time + INVOICE_TIMEOUT + BLOCK_TIME)

            # wait for invoice to expire
            self.nodes[0].setmocktime = (self.start_time + INVOICE_TIMEOUT + BLOCK_TIME + 1)
            self.nodes[0].generate(10)

            # A commits the redeem tx to complete the uncooperative close of the channel and sweep the htlc
            txid = self.commit(redeem_tx)

        else:
            # B collects the settled amount and uses the secret to sweep an unsettled htlc
            redeem_tx = B.uncooperatively_close(A, settle2_tx)

            # B commits the redeem tx to complete the uncooperative close of the channel
            txid = self.commit(redeem_tx)

            # A collects the settled amount, and has no unsettled htlcs to collect
            redeem_tx = A.uncooperatively_close(B, settle2_tx, settled_only=True)

            # A commits the redeem tx to complete the uncooperative close of the channel
            txid = self.commit(redeem_tx)

    def test2(self):

        # topology: node1 opens a channel with node 2
        A = L2Node(1)
        B = L2Node(2)
        C = L2Node(3)

        keys = {}
        witness = {}
        other_witness = {}
        setup_tx = {}
        refund_tx = {}

        '-------------------------'

        # create the set of keys needed to open a new payment channel
        keys[A], witness[A] = A.propose_channel()

        # create a new channel and sign the initial refund of a new payment channel
        other_witness[B] = B.JoinChannel(channel_partner=A, witness=witness[A])

        # create a new channel
        setup_tx[A], refund_tx[A] = A.create_channel(channel_partner=B, keys=keys[A], witness=witness[A], other_witness=other_witness[B])

        # fund and commit the setup tx to create the new channel
        self.fund(tx=setup_tx[A], spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        txid = self.commit(setup_tx[A])

        '-------------------------'

        # create the set of keys needed to open a new payment channel
        keys[B], witness[B] = B.propose_channel()

        # create a new channel and sign the initial refund of a new payment channel
        other_witness[C] = C.JoinChannel(channel_partner=B, witness=witness[B])

        # create a new channel
        setup_tx[B], refund_tx[B] = B.create_channel(channel_partner=C, keys=keys[B], witness=witness[B], other_witness=other_witness[C])

        # fund and commit the setup tx to create the new channel
        self.fund(tx=setup_tx[B], spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        txid = self.commit(setup_tx[B])

        # mine the setup tx into a new block
        self.nodes[0].generate(1)
        '-------------------------'

        invoice = {}

        # A offers to pay B the amount C requested in exchange for the secret that proves C has been paid
        invoice[A] = C.create_invoice('test2', amount=5000, expiry=self.start_time + INVOICE_TIMEOUT)

        # B offers to pay C the amount A offers (less relay fee) in exchange for the secret that proves that C has been paid
        invoice[B] = invoice[A]
        invoice[B].amount -= RELAY_FEE
        '-------------------------'

        update_tx = {}
        settle_tx = {}
        redeem_tx = {}
        other_settle_key = {}
        secret = {}

        # A creates partially signed transactions for B to pay the invoice from C
        update_tx[A], settle_tx[A], other_settle_key[A] = A.propose_payment(B, invoice[A], setup_tx[A])

        # B receives a payment commitment from A and returns the corresponding signed transactions, but not the secret needed to redeem the payment
        update_tx[B], settle_tx[B], secret[B] = B.receive_payment(A, update_tx[A], settle_tx[A], other_settle_key[A])
        assert secret[B] == None

        # B creates partially signed transactions to pay the invoice from C
        tmp_update_tx, tmp_settle_tx, tmp_other_settle_key = B.propose_payment(C, invoice[B], setup_tx[B])

        # C receives a payment commitment from B and returns the corresponding fully signed transactions and secret needed to redeem the payment
        update_tx[C], settle_tx[C], secret[C] = C.receive_payment(B, tmp_update_tx, tmp_settle_tx, tmp_other_settle_key)
        assert secret[C] != None

        # fund the transaction fees for the update tx before committing it
        self.fund(tx=update_tx[B], spend_tx=setup_tx[A], amount=FEE_AMOUNT)

        # B commits the update tx to start the uncooperative close of the channel
        txid = self.commit(update_tx[B])

        # mine the update tx into a new block, and advance blocks until settle tx can be spent
        self.nodes[0].generate(CSV_DELAY + 10)
        '-------------------------'

        # fund the transaction fees for the settle tx before committing it
        self.fund(tx=settle_tx[B], spend_tx=update_tx[B], amount=FEE_AMOUNT)

        # B commits the settle tx to finalize the uncooperative close of the channel 
        txid = self.commit(settle_tx[B])
        '-------------------------'

        # B associates the wrong preimage secret with the preimage hash
        B.secrets[invoice[B].preimage_hash] = b''

        # B tries to collect the settled htlc amount without the correct secret
        redeem_tx[B] = B.uncooperatively_close(A, settle_tx[B], settled_only=False, include_invalid=True, block_time=self.start_time + INVOICE_TIMEOUT)

        # B commits the redeem tx to complete the uncooperative close of the channel and collect settled and confirmed utxos (less transaction fees)
        txid = self.commit(redeem_tx[B], error_code=-26, error_message="non-mandatory-script-verify-flag")

        # B learns the secret from C
        assert hash160(secret[C]) == invoice[B].preimage_hash
        B.learn_secret(secret[C])
        '-------------------------'

        # B collects the settled amount and uses the secret to sweep an unsettled htlc
        redeem_tx[B] = B.uncooperatively_close(A, settle_tx[B])

        # B commits the redeem tx to complete the uncooperative close of the channel and collect settled and confirmed utxos (less transaction fees)
        txid = self.commit(redeem_tx[B])

        # A collects the settled amount, and has no unsettled htlcs to collect
        redeem_tx[A] = A.uncooperatively_close(B, settle_tx[B], settled_only=True, include_invalid=True, block_time=self.start_time + INVOICE_TIMEOUT)

        # A commits the redeem tx to complete the uncooperative close of the channel
        txid = self.commit(redeem_tx[A])

    def uncooperative_close(self, payment_sender, payment_receiver, spend_tx, update_tx, settle_tx, block_time):
        # do an uncooperative close using secrets to settle htlcs directly on the blockchain with last signed payment from B

        # fund the transaction fees for the update tx before committing it
        self.fund(tx=update_tx, spend_tx=spend_tx, amount=FEE_AMOUNT)

        # either side commits a signed update tx to start the uncooperative close of the channel
        txid = self.commit(update_tx)

        # mine the update tx into a new block, and advance blocks until settle tx can be spent
        self.nodes[0].setmocktime = block_time
        self.nodes[0].generate(CSV_DELAY + 10)
        '-------------------------'

        # fund the transaction fees for the settle tx before committing it
        self.fund(tx=settle_tx, spend_tx=update_tx, amount=FEE_AMOUNT)

        # either side commits a signed settle tx to finalize the uncooperative close of the channel 
        txid = self.commit(settle_tx)
        '-------------------------'

        # payment receiver collects their settled payments and uses their secrets to sweep any unsettled htlcs
        redeem_tx = payment_receiver.uncooperatively_close(payment_sender, settle_tx)

        # payment receiver commits the redeem tx to complete the uncooperative close of the channel and collect settled and confirmed utxos (less transaction fees)
        txid = self.commit(redeem_tx)

        # payment sender collects the settled refund amount, and any unsettled htlcs that have expired
        redeem_tx = payment_sender.uncooperatively_close(payment_receiver, settle_tx, settled_only=False, include_invalid=False, block_time=block_time + INVOICE_TIMEOUT)

        # A commits the redeem tx to complete the uncooperative close of the channel
        self.nodes[0].setmocktime = (block_time + INVOICE_TIMEOUT)
        txid = self.commit(redeem_tx)

        return block_time

    def test3(self):

        block_time = self.start_time

        # topology: node1 opens a channel with node 2
        A = L2Node(1)
        B = L2Node(2)
        C = L2Node(3)

        keys = {}
        witness = {}
        other_witness = {}
        setup_tx = {}
        refund_tx = {}
        '-------------------------'

        # create the set of keys needed to open a new payment channel
        keys[A], witness[A] = A.propose_channel()

        # create a new channel and sign the initial refund of a new payment channel
        other_witness[B] = B.JoinChannel(channel_partner=A, witness=witness[A])

        # create a new channel
        setup_tx[A], refund_tx[A] = A.create_channel(channel_partner=B, keys=keys[A], witness=witness[A], other_witness=other_witness[B])

        # fund and commit the setup tx to create the new channel
        self.fund(tx=setup_tx[A], spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        txid = self.commit(setup_tx[A])
        '-------------------------'

        # create the set of keys needed to open a new payment channel
        keys[B], witness[B] = B.propose_channel()

        # create a new channel and sign the initial refund of a new payment channel
        other_witness[C] = C.JoinChannel(channel_partner=B, witness=witness[B])

        # create a new channel
        setup_tx[B], refund_tx[B] = B.create_channel(channel_partner=C, keys=keys[B], witness=witness[B], other_witness=other_witness[C])

        # fund and commit the setup tx to create the new channel
        self.fund(tx=setup_tx[B], spend_tx=None, amount=CHANNEL_AMOUNT + FEE_AMOUNT)
        txid = self.commit(setup_tx[B])

        # mine the setup tx into a new block
        self.nodes[0].generate(1)
        '-------------------------'

        for loop in range(5):
            invoice = {}

            # A offers to pay B the amount C requested in exchange for the secret that proves C has been paid
            invoice[A] = C.create_invoice('test2', amount=5000, expiry=block_time + INVOICE_TIMEOUT)

            # B offers to pay C the amount A offers (less relay fee) in exchange for the secret that proves that C has been paid
            invoice[B] = invoice[A]
            invoice[B].amount -= RELAY_FEE
            '-------------------------'

            update1_tx = {}
            settle1_tx = {}
            update2_tx = {}
            settle2_tx = {}
            redeem_tx = {}
            other_settle_key = {}
            secret = {}

            # A creates partially signed transactions for B to pay the invoice from C
            update1_tx[A], settle1_tx[A], other_settle_key[A] = A.propose_payment(B, invoice[A], setup_tx[A])

            # B receives a payment commitment from A and returns the corresponding signed transactions, but not the secret needed to redeem the payment
            update2_tx[B], settle2_tx[B], secret[B] = B.receive_payment(A, update1_tx[A], settle1_tx[A], other_settle_key[A])
            assert secret[B] is None

            # B creates partially signed transactions to pay the invoice from C (less their relay fee)
            update1_tx[B], settle1_tx[B], other_settle_key[B] = B.propose_payment(C, invoice[B], setup_tx[B])

            # C receives a payment commitment from B and returns the corresponding fully signed transactions and secret needed to redeem the payment
            update2_tx[C], settle2_tx[C], secret[C] = C.receive_payment(B, update1_tx[B], settle1_tx[B], other_settle_key[B])
            assert hash160(secret[C]) == invoice[B].preimage_hash

            # C proposes to B to update their settled balance instead of doing an uncooperative close
            tmp_update_tx, tmp_settle_tx, settle_key, secrets = C.propose_update(B, block_time=block_time)

            # B accepts the secrets as proof of C's new settled payments balance
            tmp_update_tx, tmp_settle_tx = B.accept_update(C, tmp_update_tx, tmp_settle_tx, settle_key, secrets, block_time)

            if C.confirm_update(B, tmp_update_tx, tmp_settle_tx):
                # after C confirms that B signed the new update tx and settle tx, C does not need to uncooperatively close the channel
                update2_tx[C] = tmp_update_tx
                settle2_tx[C] = tmp_settle_tx
                block_time += 1

            else:
                # assumes atleast one payment succeeded
                assert update_tx.get(C) is not None and settle_tx(C) is not None

                # otherwise, C can uncooperatively close the channel from the last signed state
                block_time = self.uncooperative_close(B, C, setup_tx[B], update_tx[C], settle_tx[C], block_time)

            # B proposes to A to update their settled balance instead of doing an uncooperative close
            tmp_update_tx, tmp_settle_tx, settle_key, secrets = B.propose_update(A, block_time=block_time)

            # A accepts the secrets as proof of B's new settled payments balance
            tmp_update_tx, tmp_settle_tx = A.accept_update(B, tmp_update_tx, tmp_settle_tx, settle_key, secrets, block_time)

            if B.confirm_update(A, tmp_update_tx, tmp_settle_tx):
                # confirmed that A signed the new update tx and settle tx, no need to uncooperatively close the channel
                update2_tx[B] = tmp_update_tx
                settle2_tx[B] = tmp_settle_tx
                block_time += 1

            else:
                block_time = self.uncooperative_close(A, B, setup_tx[A], update2_tx[B], settle2_tx[B], block_time)

        close_tx = A.propose_close(B, setup_tx[A])
        close_tx = B.accept_close(A, close_tx, setup_tx[A])

        # if B does not sign and submit close_tx, then A should do an uncooperative close
        txid = self.commit(close_tx)

        close_tx = B.propose_close(C, setup_tx[B])
        close_tx = C.accept_close(B, close_tx, setup_tx[B])

        # if C does not sign and submit close_tx, then B should do an uncooperative close
        txid = self.commit(close_tx)

    def run_test(self):
        # create some coinbase txs to spend
        self.init_coinbase()

        # test bitcoinops workshop 2-of-2 csa tapscript tx
        self.test_bitcoinops_workshop_tapscript()

        # test eltoo tapscript tx
        self.test_tapscript_eltoo()

        # test two nodes performing an uncooperative close with one htlc pending, tested cheats:
        # - payer tries to commit a refund tx before the CSV delay expires
        # - payer tries to commit spend an update tx to an update tx with an earlier state
        # - payer tries to redeem HTLC before invoice expires
        # TODO: self.test1(payer_sweeps_utxo=True)

        # test two nodes performing an uncooperative close with one htlc pending, tested cheats:
        # - payee tries to settle last state before the CSV timeout
        # - payee tries to redeem HTLC with wrong secret
        # TODO: self.test1(payer_sweeps_utxo=False)

        # test node A paying node C via node B, node B uses secret passed by C to uncooperatively close the channel with C
        # - test cheat of node A using old update
        # - test cheat of B replacing with a newer one
        # TODO: self.test2()

        # test node A paying node C via node B, node B uses secret passed by C to cooperative update their channels
        # - test cooperative channel updating
        # - test cooperative channel closing
        # TODO: self.test3()


if __name__ == '__main__':
    SimulateL2Tests().main()
