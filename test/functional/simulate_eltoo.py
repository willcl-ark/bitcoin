#!/usr/bin/env python3
# Copyright (c) 2015-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Simulation tests for eltoo payment channel update scheme
"""

import copy
from test_framework.blocktools import (
    create_block,
    create_coinbase
)
from test_framework.key import ECKey, ECPubKey, SECP256K1_ORDER
from test_framework.messages import (
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
    CScript,
    CScriptNum,
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
    OP_RETURN,
    OP_TRUE,
    SIGHASH_ALL,
    SIGHASH_ANYPREVOUT,
    SIGHASH_NONE,
    SIGHASH_SINGLE,
    SIGHASH_ANYONECANPAY,
    SIGHASH_ANYPREVOUT,
    SIGHASH_ANYPREVOUTANYSCRIPT,
    hash160,
    hash256,
    sha256
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_raises_rpc_error
)
import time
import random

NUM_OUTPUTS_TO_COLLECT = 33
CSV_DELAY = 20
DUST_LIMIT = 800
FEE_AMOUNT = 1000
CHANNEL_AMOUNT = 1000000
RELAY_FEE = 100
NUM_SIGNERS = 2
CLTV_START_TIME = 500000000
INVOICE_TIMEOUT = 3600 # 60 minute
BLOCK_TIME = 600 # 10 minutes

def int_to_bytes(x) -> bytes:
    return x.to_bytes((x.bit_length() + 7) // 8, 'big')

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
            CScriptNum(CLTV_START_TIME+state), OP_CHECKLOCKTIMEVERIFY,
        OP_ENDIF,
    ])

def get_eltoo_update_script_witness(witness_program, is_update, witness, other_witness):
    script_witness = CScriptWitness()
    if (is_update):
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
    if preimage != None:
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

    def SetPK(self, keys):
        self.update_pk = keys.update_key.get_pubkey().get_bytes()
        self.settle_pk = keys.settle_key.get_pubkey().get_bytes()
        self.payment_pk = keys.payment_key.get_pubkey().get_bytes()

    def __repr__(self):
        return "Witness(update_pk=%064x settle_pk=%064x payment_pk=%064x)" % (self.update_pk, self.settle_pk, self.payment_pk)

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
    
    def TotalOfferedPayments(self):
        total = 0
        for key, value in self.offered_payments.items():
            total += value.amount
        return total

    def TotalReceivedPayments(self):
        total = 0
        for key, value in self.received_payments.items():
            total += value.amount
        return total

    def __repr__(self):
        return "PaymentChannel(spending_tx=%064x settled_refund_amount=%i settled_payment_amount=%i offered_payments=%i received_payments=%i)" % (self.spending_tx, self.settled_refund_amount, 
            self.settled_payment_amount, self.TotalOfferedPayments(), self.TotalReceivedPayments())

class UpdateTx(CTransaction):
    __slots__ = ("state", "witness", "other_witness")
    
    def __init__(self, payment_channel):
        super().__init__(tx=None)

        #   keep a copy of initialization parameters
        self.state = payment_channel.state
        self.witness = copy.copy(payment_channel.witness)
        self.other_witness = copy.copy(payment_channel.other_witness)

        #   set tx version 2 for BIP-68 outputs with relative timelocks
        self.nVersion = 2

        #   initialize channel state
        self.nLockTime = CLTV_START_TIME + self.state

        #   build witness program
        witness_program = get_eltoo_update_script(self.state, self.witness, self.other_witness)
        witness_hash = sha256(witness_program)
        script_wsh = CScript([OP_0, witness_hash])

        #   add channel output
        self.vout = [ CTxOut(CHANNEL_AMOUNT, script_wsh) ] # channel balance

    def Sign(self, keys):

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append( CTxIn(outpoint = COutPoint(prevscript, 0), scriptSig = b"", nSequence=0xFFFFFFFE) )

        tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, SIGHASH_ANYPREVOUT | SIGHASH_SINGLE, CHANNEL_AMOUNT)
        signature = keys.update_key.sign_ecdsa(tx_hash) + chr(SIGHASH_ANYPREVOUT | SIGHASH_SINGLE).encode('latin-1')
        
        # remove dummy vin
        self.vin.pop()

        return signature

    def Verify(self):
        verified = True
        witnesses = [ self.witness, self.other_witness ]

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append( CTxIn(outpoint = COutPoint(prevscript, 0), scriptSig = b"", nSequence=0xFFFFFFFE) )

        for witness in witnesses:
            pk = ECPubKey()
            pk.set( witness.update_pk )
            sig = witness.update_sig[0:-1]
            sighash = witness.update_sig[-1]
            assert(sighash == (SIGHASH_ANYPREVOUT | SIGHASH_SINGLE))
            tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa( sig, tx_hash )
            if v == False:
                verified = False

        # remove dummy vin
        self.vin.pop()
        
        return verified

    def AddWitness(self, spend_tx):
        # witness script to spend update tx to update tx
        self.wit.vtxinwit = [ CTxInWitness() ]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.witness.update_sig
        sig2 = self.other_witness.update_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, witness_program]
        assert(len(self.vin) == 0)
        self.vin = [ CTxIn(outpoint = COutPoint(spend_tx.sha256, 0), scriptSig = b"", nSequence=0xFFFFFFFE) ]

class SettleTx(CTransaction):
    __slots__ = ("payment_channel")

    def __init__(self, payment_channel):
        super().__init__(tx=None)

        self.payment_channel = copy.deepcopy(payment_channel)

        #   set tx version 2 for BIP-68 outputs with relative timelocks
        self.nVersion = 2

        #   initialize channel state
        self.nLockTime = CLTV_START_TIME + self.payment_channel.state

        #   build witness program
        witness_program = get_eltoo_update_script(self.payment_channel.state, self.payment_channel.witness, self.payment_channel.other_witness)
        witness_hash = sha256(witness_program)
        script_wsh = CScript([OP_0, witness_hash])

        assert self.payment_channel.settled_refund_amount + self.payment_channel.settled_payment_amount + self.payment_channel.TotalOfferedPayments() - CHANNEL_AMOUNT == 0
        settled_amounts = [ self.payment_channel.settled_refund_amount, self.payment_channel.settled_payment_amount ]
        signers = [ self.payment_channel.witness.payment_pk, self.payment_channel.other_witness.payment_pk ]
        signer_index = 0
        outputs = []
        for amount in settled_amounts:
            if amount > DUST_LIMIT:
                #   pay to new p2pkh outputs, TODO: should use p2wpkh
                payment_pk = signers[signer_index]
                script_pkh = CScript([OP_0, hash160(payment_pk)])
                #self.log.debug("add_settle_outputs: state=%s, signer_index=%d, witness hash160(%s)\n", state, signer_index, ToHex(settlement_pubkey))
                outputs.append(CTxOut(amount, script_pkh))
            signer_index+=1

        for htlc_hash, htlc in self.payment_channel.offered_payments.items():
            if htlc.amount > DUST_LIMIT:
                #   refund and pay to p2pkh outputs, TODO: should use p2wpkh
                refund_pubkey = self.payment_channel.witness.payment_pk
                payment_pubkey = self.payment_channel.other_witness.payment_pk
                preimage_hash = self.payment_channel.offered_payments[htlc_hash].preimage_hash
                expiry = self.payment_channel.offered_payments[htlc_hash].expiry
                
                #   build witness program
                witness_program = get_eltoo_htlc_script(refund_pubkey, payment_pubkey, preimage_hash, expiry)
                witness_hash = sha256(witness_program)
                script_wsh = CScript([OP_0, witness_hash])
                #self.log.debug("add_settle_outputs: state=%s, signer_index=%d\n\twitness sha256(%s)=%s\n\twsh sha256(%s)=%s\n", state, signer_index, ToHex(witness_program),
                #    ToHex(witness_hash), ToHex(script_wsh), ToHex(sha256(script_wsh)))
                outputs.append(CTxOut(htlc.amount, script_wsh))

        #   add settlement outputs to settlement transaction
        self.vout = outputs

    def Sign(self, keys):
        # TODO: spending from a SetupTx (first UpdateTx) should not use the NOINPUT sighash 

        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append( CTxIn(outpoint = COutPoint(prevscript, 0), scriptSig = b"", nSequence=CSV_DELAY) )

        tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, SIGHASH_ANYPREVOUT | SIGHASH_SINGLE, CHANNEL_AMOUNT)
        signature = keys.settle_key.sign_ecdsa(tx_hash) + chr(SIGHASH_ANYPREVOUT | SIGHASH_SINGLE).encode('latin-1')

        # remove dummy vin
        self.vin.pop()

        return signature

    def Verify(self):
        verified = True
        witnesses = [ self.payment_channel.witness, self.payment_channel.other_witness ]
        
        # add dummy vin, digest only serializes the nSequence value
        prevscript = CScript()
        self.vin.append( CTxIn(outpoint = COutPoint(prevscript, 0), scriptSig = b"", nSequence=CSV_DELAY) )

        for witness in witnesses:
            pk = ECPubKey()
            pk.set( witness.settle_pk )
            sig = witness.settle_sig[0:-1]
            sighash = witness.settle_sig[-1]
            assert(sighash == (SIGHASH_ANYPREVOUT | SIGHASH_SINGLE))
            tx_hash = SegwitVersion1SignatureHash(prevscript, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa( sig, tx_hash )
            verified = verified and pk.verify_ecdsa( sig, tx_hash )

        # remove dummy vin
        self.vin.pop()
        
        return verified

    def AddWitness(self, spend_tx):
        # witness script to spend update tx to settle tx
        assert spend_tx.state == self.payment_channel.state
        self.wit.vtxinwit = [ CTxInWitness() ]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.payment_channel.witness.settle_sig
        sig2 = self.payment_channel.other_witness.settle_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, b'', b'', b'', witness_program]
        assert(len(self.vin) == 0)
        self.vin = [ CTxIn(outpoint = COutPoint(spend_tx.sha256, 0), scriptSig = b"", nSequence=CSV_DELAY) ]

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
        assert(settled_amount > FEE_AMOUNT)

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
        self.vout = [ CTxOut(settled_amount, script_pkh) ] # channel balance

    def Sign(self, keys, htlc_index, htlc_hash):
        
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
            if self.is_funder == True:
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

    def AddWitness(self, keys, spend_tx, settled_only):
        if self.is_funder:
            signer_index=0
        else:
            signer_index=1
        settled_amounts = [ self.payment_channel.settled_refund_amount, self.payment_channel.settled_payment_amount ]

        # add settled input from htlc sender (after a timeout) or htlc receiver (with preimage)
        input_index = 0
        for amount_index in range(len(settled_amounts)) :
            if settled_amounts[amount_index] > DUST_LIMIT:
                # add input from signer
                if amount_index is signer_index:
                    self.vin.append( CTxIn(outpoint = COutPoint(spend_tx.sha256, input_index), scriptSig = b"", nSequence=0xfffffffe) )
                input_index += 1
            
        if not settled_only:
            # add htlc inputs, one per htlc
            for htlc_hash, htlc in self.payment_channel.offered_payments.items():
                if not self.include_invalid and self.is_funder and htlc.expiry > self.block_time:
                    continue
                if not self.include_invalid and not self.is_funder and htlc.preimage_hash not in self.secrets:
                    continue
                if htlc.amount > DUST_LIMIT:
                    self.vin.append( CTxIn(outpoint = COutPoint(spend_tx.sha256, input_index), scriptSig = b"", nSequence=0xfffffffe) )
                    input_index += 1

        self.wit.vtxinwit = []
        #   add the p2wpkh witness scripts to spend the settled channel amounts
        input_index = 0
        for amount_index in range(len(settled_amounts)) :
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
            #   add the p2wsh witness scripts to spend the settled channel amounts
            for htlc_hash, htlc in self.payment_channel.offered_payments.items():
                    if not self.include_invalid and self.is_funder and htlc.expiry > self.block_time:
                        continue
                    if not self.include_invalid and not self.is_funder and htlc.preimage_hash not in self.secrets:
                        continue
                    if  htlc.amount > DUST_LIMIT:
                        #   generate signature for current state 
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
            self.payment_channel.settled_payment_amount -= min(int(FEE_AMOUNT/2), self.payment_channel.settled_payment_amount)
            self.payment_channel.settled_refund_amount = CHANNEL_AMOUNT - self.payment_channel.settled_payment_amount - FEE_AMOUNT
        else:
            self.payment_channel.settled_refund_amount -= min(int(FEE_AMOUNT/2), self.payment_channel.settled_refund_amount)
            self.payment_channel.settled_payment_amount = CHANNEL_AMOUNT - self.payment_channel.settled_refund_amount - FEE_AMOUNT
        assert self.payment_channel.settled_refund_amount + self.payment_channel.settled_payment_amount + FEE_AMOUNT == CHANNEL_AMOUNT

        # no csv outputs, so nVersion can be 1 or 2
        self.nVersion = 2

        # refund outputs to channel partners immediately
        self.nLockTime = CLTV_START_TIME + self.payment_channel.state+1   

        # add setup_tx vin
        self.vin = [ CTxIn(outpoint = COutPoint(setup_tx.sha256, 0), scriptSig = b"", nSequence=0xFFFFFFFE) ]

        #   build witness program for settled refund output (p2wpkh)
        pubkey = self.payment_channel.witness.payment_pk
        script_pkh = CScript([OP_0, hash160(pubkey)])

        outputs = []

        #   refund output
        if self.payment_channel.settled_refund_amount > DUST_LIMIT:
            outputs.append( CTxOut(self.payment_channel.settled_refund_amount, script_pkh) )

        #   build witness program for settled payment output (p2wpkh)
        pubkey = self.payment_channel.other_witness.payment_pk
        script_pkh = CScript([OP_0, hash160(pubkey)])

        #   settled output
        if self.payment_channel.settled_payment_amount > DUST_LIMIT:
            outputs.append( CTxOut(self.payment_channel.settled_payment_amount, script_pkh) )

        self.vout = outputs

    def IsChannelFunder(self, keys):
        pubkey = keys.update_key.get_pubkey().get_bytes()
        if pubkey == self.payment_channel.witness.update_pk:
            return True
        else:
            return False

    def Sign(self, keys, setup_tx):
        
        # spending from a SetupTx (first UpdateTx) should not use the NOINPUT sighash 

        witness_program = get_eltoo_update_script(setup_tx.state, setup_tx.witness, setup_tx.other_witness)
        tx_hash = SegwitVersion1SignatureHash(witness_program, self, 0, SIGHASH_SINGLE, CHANNEL_AMOUNT)
        signature = keys.update_key.sign_ecdsa(tx_hash) + chr(SIGHASH_SINGLE).encode('latin-1')

        if self.IsChannelFunder(keys):
            self.payment_channel.witness.update_sig = signature
        else:
            self.payment_channel.other_witness.update_sig = signature

        return signature

    def Verify(self, setup_tx):
        verified = True
        witnesses = [ self.payment_channel.witness, self.payment_channel.other_witness ]

        for witness in witnesses:
            pk = ECPubKey()
            pk.set( witness.update_pk )
            sig = witness.update_sig[0:-1]
            sighash = witness.update_sig[-1]
            assert(sighash == (SIGHASH_SINGLE))
            witness_program = get_eltoo_update_script(setup_tx.state, setup_tx.witness, setup_tx.other_witness)
            tx_hash = SegwitVersion1SignatureHash(witness_program, self, 0, sighash, CHANNEL_AMOUNT)
            v = pk.verify_ecdsa( sig, tx_hash )
            verified = verified and pk.verify_ecdsa( sig, tx_hash )
        
        return verified

    def AddWitness(self, spend_tx):
        # witness script to spend update tx to close tx
        self.wit.vtxinwit = [ CTxInWitness() ]
        witness_program = get_eltoo_update_script(spend_tx.state, spend_tx.witness, spend_tx.other_witness)
        sig1 = self.payment_channel.witness.update_sig
        sig2 = self.payment_channel.other_witness.update_sig
        self.wit.vtxinwit[0].scriptWitness = CScriptWitness()
        self.wit.vtxinwit[0].scriptWitness.stack = [b'', sig1, sig2, witness_program]

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
        self.start_time = int(1500000000)# int(time.time())
        self.nodes[0].setmocktime=(self.start_time)

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
        assert fund_tx != None
        self.coinbase_index+=1
        fund_key = self.coinbase_key
        outIdx = 0 

        #   update vin and witness to spend a specific update tx (skipped for setup tx)
        if spend_tx != None:
            tx.AddWitness(spend_tx)

        #   pay change to new p2pkh output, TODO: should use p2wpkh
        change_key = ECKey()
        change_key.generate()
        change_pubkey = change_key.get_pubkey().get_bytes()
        change_script_pkh = CScript([OP_0, hash160(change_pubkey)])
        change_amount = fund_tx.vout[0].nValue - amount

        #   add new funding input and change output
        tx.vin.append(CTxIn(COutPoint(fund_tx.sha256, 0), b""))
        tx.vout.append(CTxOut(change_amount, change_script_pkh))

        #   pay fee from spend_tx w/change output (assumed to be last txin)
        inIdx = len(tx.vin)-1
        
        #   sign the tx fee input w/change output
        scriptPubKey = bytearray(fund_tx.vout[outIdx].scriptPubKey)
        (sighash, err) = SignatureHash(fund_tx.vout[0].scriptPubKey, tx, inIdx, SIGHASH_ALL)
        sig = fund_key.sign_ecdsa(sighash) + bytes(bytearray([SIGHASH_ALL]))
        tx.vin[inIdx].scriptSig = CScript([sig])

        #   update the hash of this transaction
        tx.rehash()

        return (change_key, change_amount)

    def commit(self, tx, error_code=None, error_message=None):
        #   update hash
        tx.rehash()

        #   confirm it is in the mempool
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

    def run_test(self):
        # create some coinbase txs to spend
        self.init_coinbase()

if __name__ == '__main__':
    SimulateL2Tests().main()
