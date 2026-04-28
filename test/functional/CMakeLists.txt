# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

if(NOT BUILD_DAEMON)
  message(STATUS "Functional tests disabled because BUILD_DAEMON=OFF")
  return()
endif()

if(NOT Python3_FOUND)
  message(STATUS "Functional tests disabled because Python3 was not found")
  return()
endif()

set(BITCOIN_FUNCTIONAL_TEST_TIMEOUT_FACTOR "1" CACHE STRING "Timeout factor passed to functional tests")
set(BITCOIN_FUNCTIONAL_TEST_ARGS "" CACHE STRING "Additional arguments passed to functional tests")
set(BITCOIN_FUNCTIONAL_TEST_COMMAND "${Python3_EXECUTABLE}" CACHE STRING "Command used to run functional tests")
set(BITCOIN_FUNCTIONAL_TEST_SCRIPT_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE STRING "Directory containing functional test scripts")
set(BITCOIN_FUNCTIONAL_TEST_CONFIG_FILE "${PROJECT_BINARY_DIR}/test/config.ini" CACHE STRING "Functional test config file")
set(BITCOIN_FUNCTIONAL_TEST_CACHE_DIR "${PROJECT_BINARY_DIR}/test/functional/cache" CACHE STRING "Functional test cache directory")
set(BITCOIN_FUNCTIONAL_TEST_COVERAGE_DIR "${PROJECT_BINARY_DIR}/test/functional/coverage" CACHE STRING "Functional test RPC coverage directory")
option(BITCOIN_FUNCTIONAL_TEST_COVERAGE "Enable functional test RPC coverage reporting" OFF)
option(BITCOIN_FUNCTIONAL_EXTENDED_TESTS "Enable extended functional tests" OFF)
separate_arguments(functional_test_command UNIX_COMMAND "${BITCOIN_FUNCTIONAL_TEST_COMMAND}")
separate_arguments(functional_test_args UNIX_COMMAND "${BITCOIN_FUNCTIONAL_TEST_ARGS}")
set(functional_test_fixtures functional_cache)

if(BITCOIN_FUNCTIONAL_TEST_COVERAGE)
  list(APPEND functional_test_args --coveragedir=${BITCOIN_FUNCTIONAL_TEST_COVERAGE_DIR})
  list(APPEND functional_test_fixtures functional_coverage)
endif()

set(FUNCTIONAL_EXTENDED_TEST_COMMANDS
  [=[feature_pruning.py]=]
  [=[feature_dbcrash.py]=]
  [=[feature_index_prune.py]=]
)

set(FUNCTIONAL_BASE_TEST_COMMANDS
  [=[tool_bench_sanity_check.py]=]
  [=[feature_fee_estimation.py]=]
  [=[feature_taproot.py]=]
  [=[feature_block.py]=]
  [=[mempool_ephemeral_dust.py]=]
  [=[wallet_conflicts.py]=]
  [=[p2p_opportunistic_1p1c.py]=]
  [=[p2p_node_network_limited.py --v1transport]=]
  [=[p2p_node_network_limited.py --v2transport]=]
  [=[mining_getblocktemplate_longpoll.py]=]
  [=[p2p_segwit.py]=]
  [=[feature_maxuploadtarget.py]=]
  [=[feature_assumeutxo.py]=]
  [=[mempool_updatefromblock.py]=]
  [=[mempool_persist.py]=]
  [=[rpc_psbt.py]=]
  [=[wallet_fundrawtransaction.py]=]
  [=[wallet_bumpfee.py]=]
  [=[wallet_v3_txs.py]=]
  [=[wallet_backup.py]=]
  [=[feature_segwit.py --v2transport]=]
  [=[feature_segwit.py --v1transport]=]
  [=[p2p_tx_download.py]=]
  [=[wallet_avoidreuse.py]=]
  [=[feature_abortnode.py]=]
  [=[wallet_address_types.py]=]
  [=[p2p_orphan_handling.py]=]
  [=[wallet_basic.py]=]
  [=[feature_maxtipage.py]=]
  [=[wallet_multiwallet.py]=]
  [=[wallet_multiwallet.py --usecli]=]
  [=[p2p_dns_seeds.py]=]
  [=[wallet_groups.py]=]
  [=[p2p_blockfilters.py]=]
  [=[feature_assumevalid.py]=]
  [=[wallet_taproot.py]=]
  [=[feature_bip68_sequence.py]=]
  [=[rpc_packages.py]=]
  [=[rpc_bind.py --ipv4]=]
  [=[rpc_bind.py --ipv6]=]
  [=[rpc_bind.py --nonloopback]=]
  [=[p2p_headers_sync_with_minchainwork.py]=]
  [=[p2p_feefilter.py]=]
  [=[feature_csv_activation.py]=]
  [=[p2p_sendheaders.py]=]
  [=[feature_config_args.py]=]
  [=[wallet_listtransactions.py]=]
  [=[wallet_miniscript.py]=]
  [=[p2p_invalid_messages.py]=]
  [=[rpc_createmultisig.py]=]
  [=[p2p_timeouts.py --v1transport]=]
  [=[p2p_timeouts.py --v2transport]=]
  [=[rpc_signer.py]=]
  [=[wallet_signer.py]=]
  [=[mempool_limit.py]=]
  [=[rpc_txoutproof.py]=]
  [=[rpc_orphans.py]=]
  [=[wallet_listreceivedby.py]=]
  [=[wallet_abandonconflict.py]=]
  [=[wallet_anchor.py]=]
  [=[feature_reindex.py]=]
  [=[feature_reindex_readonly.py]=]
  [=[wallet_labels.py]=]
  [=[p2p_compactblocks.py]=]
  [=[p2p_compactblocks_blocksonly.py]=]
  [=[wallet_hd.py]=]
  [=[wallet_blank.py]=]
  [=[wallet_keypool_topup.py]=]
  [=[wallet_fast_rescan.py]=]
  [=[wallet_gethdkeys.py]=]
  [=[wallet_createwalletdescriptor.py]=]
  [=[interface_zmq.py]=]
  [=[rpc_invalid_address_message.py]=]
  [=[rpc_validateaddress.py]=]
  [=[interface_bitcoin_cli.py]=]
  [=[feature_bind_extra.py]=]
  [=[mempool_resurrect.py]=]
  [=[wallet_txn_doublespend.py --mineblock]=]
  [=[tool_bitcoin_chainstate.py]=]
  [=[tool_wallet.py]=]
  [=[tool_utils.py]=]
  [=[tool_signet_miner.py]=]
  [=[wallet_txn_clone.py]=]
  [=[wallet_txn_clone.py --segwit]=]
  [=[rpc_getchaintips.py]=]
  [=[rpc_misc.py]=]
  [=[p2p_1p1c_network.py]=]
  [=[interface_rest.py]=]
  [=[mempool_spend_coinbase.py]=]
  [=[wallet_avoid_mixing_output_types.py]=]
  [=[mempool_reorg.py]=]
  [=[p2p_block_sync.py --v1transport]=]
  [=[p2p_block_sync.py --v2transport]=]
  [=[wallet_createwallet.py --usecli]=]
  [=[wallet_createwallet.py]=]
  [=[wallet_reindex.py]=]
  [=[wallet_reorgsrestore.py]=]
  [=[interface_http.py]=]
  [=[interface_rpc.py]=]
  [=[interface_usdt_coinselection.py]=]
  [=[interface_usdt_mempool.py]=]
  [=[interface_usdt_net.py]=]
  [=[interface_usdt_utxocache.py]=]
  [=[interface_usdt_validation.py]=]
  [=[rpc_users.py]=]
  [=[rpc_whitelist.py]=]
  [=[feature_proxy.py]=]
  [=[wallet_signrawtransactionwithwallet.py]=]
  [=[rpc_signrawtransactionwithkey.py]=]
  [=[rpc_rawtransaction.py]=]
  [=[wallet_transactiontime_rescan.py]=]
  [=[p2p_addrv2_relay.py]=]
  [=[p2p_compactblocks_hb.py --v1transport]=]
  [=[p2p_compactblocks_hb.py --v2transport]=]
  [=[p2p_disconnect_ban.py --v1transport]=]
  [=[p2p_disconnect_ban.py --v2transport]=]
  [=[feature_posix_fs_permissions.py]=]
  [=[rpc_decodescript.py]=]
  [=[rpc_blockchain.py --v1transport]=]
  [=[rpc_blockchain.py --v2transport]=]
  [=[mining_template_verification.py]=]
  [=[rpc_deprecated.py]=]
  [=[wallet_disable.py]=]
  [=[wallet_change_address.py]=]
  [=[p2p_addr_relay.py]=]
  [=[p2p_getaddr_caching.py]=]
  [=[p2p_getdata.py]=]
  [=[p2p_addrfetch.py]=]
  [=[rpc_net.py --v1transport]=]
  [=[rpc_net.py --v2transport]=]
  [=[wallet_keypool.py]=]
  [=[wallet_descriptor.py]=]
  [=[p2p_nobloomfilter_messages.py]=]
  [=[feature_framework_unit_tests.py]=]
  [=[p2p_filter.py]=]
  [=[rpc_setban.py --v1transport]=]
  [=[rpc_setban.py --v2transport]=]
  [=[p2p_blocksonly.py]=]
  [=[mining_prioritisetransaction.py]=]
  [=[p2p_invalid_locator.py]=]
  [=[p2p_invalid_block.py --v1transport]=]
  [=[p2p_invalid_block.py --v2transport]=]
  [=[p2p_invalid_tx.py --v1transport]=]
  [=[p2p_invalid_tx.py --v2transport]=]
  [=[p2p_v2_transport.py]=]
  [=[p2p_v2_encrypted.py]=]
  [=[p2p_v2_misbehaving.py]=]
  [=[example_test.py]=]
  [=[mempool_truc.py]=]
  [=[wallet_multisig_descriptor_psbt.py]=]
  [=[wallet_miniscript_decaying_multisig_descriptor_psbt.py]=]
  [=[wallet_txn_doublespend.py]=]
  [=[wallet_backwards_compatibility.py]=]
  [=[wallet_txn_clone.py --mineblock]=]
  [=[feature_notifications.py]=]
  [=[rpc_getblockfilter.py]=]
  [=[rpc_getblockfrompeer.py]=]
  [=[rpc_invalidateblock.py]=]
  [=[feature_utxo_set_hash.py]=]
  [=[feature_rbf.py]=]
  [=[mempool_packages.py]=]
  [=[mempool_package_limits.py]=]
  [=[mempool_package_rbf.py]=]
  [=[tool_utxo_to_sqlite.py]=]
  [=[feature_versionbits_warning.py]=]
  [=[feature_blocksxor.py]=]
  [=[rpc_preciousblock.py]=]
  [=[wallet_importprunedfunds.py]=]
  [=[p2p_leak_tx.py --v1transport]=]
  [=[p2p_leak_tx.py --v2transport]=]
  [=[p2p_eviction.py]=]
  [=[p2p_outbound_eviction.py]=]
  [=[p2p_ibd_stalling.py --v1transport]=]
  [=[p2p_ibd_stalling.py --v2transport]=]
  [=[p2p_net_deadlock.py --v1transport]=]
  [=[p2p_net_deadlock.py --v2transport]=]
  [=[wallet_signmessagewithaddress.py]=]
  [=[rpc_signmessagewithprivkey.py]=]
  [=[rpc_generate.py]=]
  [=[wallet_balance.py]=]
  [=[p2p_initial_headers_sync.py]=]
  [=[feature_nulldummy.py]=]
  [=[mempool_accept.py]=]
  [=[p2p_addr_selfannouncement.py]=]
  [=[mempool_expiry.py]=]
  [=[wallet_importdescriptors.py]=]
  [=[wallet_crosschain.py]=]
  [=[mining_basic.py]=]
  [=[mining_mainnet.py]=]
  [=[feature_signet.py]=]
  [=[p2p_mutated_blocks.py]=]
  [=[rpc_named_arguments.py]=]
  [=[feature_startupnotify.py]=]
  [=[wallet_simulaterawtx.py]=]
  [=[wallet_listsinceblock.py]=]
  [=[wallet_listdescriptors.py]=]
  [=[p2p_leak.py]=]
  [=[wallet_encryption.py]=]
  [=[feature_dersig.py]=]
  [=[feature_reindex_init.py]=]
  [=[feature_cltv.py]=]
  [=[rpc_uptime.py]=]
  [=[feature_discover.py]=]
  [=[wallet_resendwallettransactions.py]=]
  [=[wallet_fallbackfee.py]=]
  [=[rpc_dumptxoutset.py]=]
  [=[feature_minchainwork.py]=]
  [=[rpc_estimatefee.py]=]
  [=[p2p_private_broadcast.py]=]
  [=[rpc_getblockstats.py]=]
  [=[feature_port.py]=]
  [=[feature_bind_port_externalip.py]=]
  [=[wallet_create_tx.py]=]
  [=[wallet_send.py]=]
  [=[wallet_sendall.py]=]
  [=[wallet_sendmany.py]=]
  [=[wallet_spend_unconfirmed.py]=]
  [=[wallet_rescan_unconfirmed.py]=]
  [=[p2p_fingerprint.py]=]
  [=[feature_uacomment.py]=]
  [=[feature_init.py]=]
  [=[wallet_coinbase_category.py]=]
  [=[feature_filelock.py]=]
  [=[feature_loadblock.py]=]
  [=[wallet_assumeutxo.py]=]
  [=[p2p_add_connections.py]=]
  [=[feature_bind_port_discover.py]=]
  [=[p2p_unrequested_blocks.py]=]
  [=[p2p_message_capture.py]=]
  [=[feature_includeconf.py]=]
  [=[feature_addrman.py]=]
  [=[feature_asmap.py]=]
  [=[feature_chain_tiebreaks.py]=]
  [=[feature_fastprune.py]=]
  [=[feature_framework_miniwallet.py]=]
  [=[mempool_unbroadcast.py]=]
  [=[mempool_compatibility.py]=]
  [=[mempool_accept_wtxid.py]=]
  [=[mempool_dust.py]=]
  [=[mempool_sigoplimit.py]=]
  [=[rpc_deriveaddresses.py]=]
  [=[rpc_deriveaddresses.py --usecli]=]
  [=[p2p_ping.py]=]
  [=[p2p_tx_privacy.py]=]
  [=[rpc_getdescriptoractivity.py]=]
  [=[rpc_scanblocks.py]=]
  [=[tool_bitcoin.py]=]
  [=[p2p_sendtxrcncl.py]=]
  [=[rpc_scantxoutset.py]=]
  [=[feature_torcontrol.py]=]
  [=[feature_unsupported_utxo_db.py]=]
  [=[mempool_cluster.py]=]
  [=[feature_logging.py]=]
  [=[interface_ipc.py]=]
  [=[interface_ipc_mining.py]=]
  [=[feature_anchors.py]=]
  [=[mempool_datacarrier.py]=]
  [=[feature_coinstatsindex.py]=]
  [=[feature_coinstatsindex_compatibility.py]=]
  [=[wallet_orphanedreward.py]=]
  [=[wallet_musig.py]=]
  [=[wallet_timelock.py]=]
  [=[p2p_permissions.py]=]
  [=[feature_blocksdir.py]=]
  [=[wallet_startup.py]=]
  [=[feature_remove_pruned_files_on_startup.py]=]
  [=[p2p_i2p_ports.py]=]
  [=[p2p_i2p_sessions.py]=]
  [=[feature_presegwit_node_upgrade.py]=]
  [=[feature_settings.py]=]
  [=[rpc_getdescriptorinfo.py]=]
  [=[rpc_gettxspendingprevout.py]=]
  [=[rpc_help.py]=]
  [=[feature_framework_testshell.py]=]
  [=[tool_rpcauth.py]=]
  [=[p2p_handshake.py]=]
  [=[p2p_handshake.py --v2transport]=]
  [=[interface_ipc_cli.py]=]
  [=[feature_dirsymlinks.py]=]
  [=[feature_help.py]=]
  [=[feature_framework_startup_failures.py]=]
  [=[feature_shutdown.py]=]
  [=[wallet_migration.py]=]
  [=[p2p_ibd_txrelay.py]=]
  [=[p2p_seednode.py]=]
)

set(FUNCTIONAL_TEST_COMMANDS ${FUNCTIONAL_BASE_TEST_COMMANDS})
if(BITCOIN_FUNCTIONAL_EXTENDED_TESTS)
  list(PREPEND FUNCTIONAL_TEST_COMMANDS ${FUNCTIONAL_EXTENDED_TEST_COMMANDS})
endif()
set(FUNCTIONAL_ALL_TEST_COMMANDS ${FUNCTIONAL_EXTENDED_TEST_COMMANDS} ${FUNCTIONAL_BASE_TEST_COMMANDS})

set(FUNCTIONAL_TEST_HELPERS
  combine_logs.py
  create_cache.py
  coverage_report.py
  coverage_setup.py
)

function(get_functional_test_script command output_var)
  separate_arguments(command_parts UNIX_COMMAND "${command}")
  list(GET command_parts 0 script)
  set(${output_var} "${script}" PARENT_SCOPE)
endfunction()

set(functional_test_scripts)
foreach(test_command IN LISTS FUNCTIONAL_ALL_TEST_COMMANDS)
  get_functional_test_script("${test_command}" script)
  list(APPEND functional_test_scripts "${script}")
endforeach()
list(REMOVE_DUPLICATES functional_test_scripts)
list(SORT functional_test_scripts)

file(GLOB all_functional_scripts
  LIST_DIRECTORIES FALSE
  RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  *.py
)
foreach(helper IN LISTS FUNCTIONAL_TEST_HELPERS)
  list(REMOVE_ITEM all_functional_scripts "${helper}")
endforeach()
list(SORT all_functional_scripts)

if(NOT all_functional_scripts STREQUAL functional_test_scripts)
  set(unregistered_scripts ${all_functional_scripts})
  list(REMOVE_ITEM unregistered_scripts ${functional_test_scripts})
  set(missing_scripts ${functional_test_scripts})
  list(REMOVE_ITEM missing_scripts ${all_functional_scripts})
  message(FATAL_ERROR "Functional test registration mismatch. Unregistered scripts: ${unregistered_scripts}. Missing scripts: ${missing_scripts}")
endif()

add_test(NAME functional.create_cache
  COMMAND ${functional_test_command}
    ${BITCOIN_FUNCTIONAL_TEST_SCRIPT_DIR}/create_cache.py
    --configfile=${BITCOIN_FUNCTIONAL_TEST_CONFIG_FILE}
    --cachedir=${BITCOIN_FUNCTIONAL_TEST_CACHE_DIR}
    --portseed=1
)
set_tests_properties(functional.create_cache PROPERTIES
  FIXTURES_SETUP functional_cache
  SKIP_RETURN_CODE 77
  ENVIRONMENT PYTHON_GIL=1
)

if(BITCOIN_FUNCTIONAL_TEST_COVERAGE)
  add_test(NAME functional.coverage_setup
    COMMAND ${functional_test_command}
      ${BITCOIN_FUNCTIONAL_TEST_SCRIPT_DIR}/coverage_setup.py
      --coveragedir=${BITCOIN_FUNCTIONAL_TEST_COVERAGE_DIR}
  )
  set_tests_properties(functional.coverage_setup PROPERTIES
    FIXTURES_SETUP functional_coverage
    ENVIRONMENT PYTHON_GIL=1
  )

  add_test(NAME functional.coverage_report
    COMMAND ${functional_test_command}
      ${BITCOIN_FUNCTIONAL_TEST_SCRIPT_DIR}/coverage_report.py
      --coveragedir=${BITCOIN_FUNCTIONAL_TEST_COVERAGE_DIR}
  )
  set_tests_properties(functional.coverage_report PROPERTIES
    FIXTURES_CLEANUP functional_coverage
    ENVIRONMENT PYTHON_GIL=1
  )
endif()

set(portseed 1)
foreach(test_command IN LISTS FUNCTIONAL_TEST_COMMANDS)
  math(EXPR portseed "${portseed} + 1")
  separate_arguments(command_parts UNIX_COMMAND "${test_command}")
  list(GET command_parts 0 script)
  list(REMOVE_AT command_parts 0)

  string(REGEX REPLACE "[.]py$" "" test_name "${script}")
  foreach(test_arg IN LISTS command_parts)
    string(REGEX REPLACE "^--" "" test_arg_name "${test_arg}")
    string(REGEX REPLACE "[^A-Za-z0-9_]+" "." test_arg_name "${test_arg_name}")
    string(REGEX REPLACE "^[.]" "" test_arg_name "${test_arg_name}")
    string(REGEX REPLACE "[.]$" "" test_arg_name "${test_arg_name}")
    string(APPEND test_name ".${test_arg_name}")
  endforeach()

  add_test(NAME "functional.${test_name}"
    COMMAND ${functional_test_command}
      ${BITCOIN_FUNCTIONAL_TEST_SCRIPT_DIR}/${script}
      ${command_parts}
      ${functional_test_args}
      --configfile=${BITCOIN_FUNCTIONAL_TEST_CONFIG_FILE}
      --cachedir=${BITCOIN_FUNCTIONAL_TEST_CACHE_DIR}
      --portseed=${portseed}
      --timeout-factor=${BITCOIN_FUNCTIONAL_TEST_TIMEOUT_FACTOR}
  )
  set_tests_properties("functional.${test_name}" PROPERTIES
    FIXTURES_REQUIRED "${functional_test_fixtures}"
    SKIP_RETURN_CODE 77
    ENVIRONMENT PYTHON_GIL=1
  )
endforeach()
