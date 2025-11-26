# Functional test definitions
# Lists all functional tests with proper grouping and cost assignments

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Detect multi-config generators (MSVC, Xcode) vs single-config (Ninja, Makefiles)
# Multi-config generators place binaries in bin/<CONFIG>/, single-config in bin/
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT DEFINED ENABLE_WALLET)
    set(ENABLE_WALLET ON)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(IS_LINUX TRUE)
else()
    set(IS_LINUX FALSE)
endif()

option(ENABLE_EXTENDED_FUNCTIONAL_TESTS "Enable extended/slow functional tests" OFF)
option(ENABLE_PREVIOUS_RELEASES_TESTS "Enable tests requiring previous Bitcoin releases" OFF)
option(ENABLE_USDT_TESTS "Enable USDT tracepoint tests (requires Linux + BPF permissions)" OFF)
option(ENABLE_SPECIAL_NETWORK_TESTS "Enable tests requiring special network configuration" OFF)
option(ENABLE_COVERAGE "Enable RPC coverage tracking for functional tests" OFF)

# Helper to add functional tests
function(add_functional_test test_name test_script)
    set(options "")
    set(oneValueArgs COST WORKING_DIRECTORY)
    set(multiValueArgs LABELS DEPENDS)
    cmake_parse_arguments(AFT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AFT_WORKING_DIRECTORY)
        set(AFT_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    if(NOT AFT_COST)
        set(AFT_COST 1)
    endif()

    # Parse script name and arguments
    string(REPLACE " " ";" test_parts "${test_script}")
    list(GET test_parts 0 script_name)
    list(LENGTH test_parts num_parts)

    # Build base command arguments
    set(base_args --cachedir=${CMAKE_CURRENT_BINARY_DIR}/test/cache)

    # Add coverage directory if coverage is enabled
    if(ENABLE_COVERAGE)
        list(APPEND base_args --coveragedir=${CMAKE_CURRENT_BINARY_DIR}/coverage)
    endif()

    if(num_parts GREATER 1)
      # We've got arguments
        list(SUBLIST test_parts 1 -1 script_args)
        add_test(
            NAME "bitcoin.functional.${test_name}"
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/${script_name} ${base_args} ${script_args}
            WORKING_DIRECTORY ${AFT_WORKING_DIRECTORY}
        )
    else()
        add_test(
            NAME "bitcoin.functional.${test_name}"
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/${script_name} ${base_args}
            WORKING_DIRECTORY ${AFT_WORKING_DIRECTORY}
        )
    endif()

    set_tests_properties("bitcoin.functional.${test_name}" PROPERTIES
        LABELS "functional"
        COST ${AFT_COST}
        FIXTURES_REQUIRED "functional_cache"
        ENVIRONMENT "CMAKE_CONFIG=$<$<BOOL:${is_multi_config}>:$<CONFIG>>"
    )

    if(AFT_LABELS)
        set_tests_properties("bitcoin.functional.${test_name}" PROPERTIES
            LABELS "functional;${AFT_LABELS}"
        )
    endif()
endfunction()

# Extended/slow tests (>5 minutes) - COST 300+
if(ENABLE_EXTENDED_FUNCTIONAL_TESTS)
    add_functional_test(feature_pruning "feature_pruning.py" COST 500)
    add_functional_test(feature_dbcrash "feature_dbcrash.py" COST 400)
    add_functional_test(wallet_multiwallet_rescan "wallet_multiwallet.py --descriptors --rescan" COST 350 LABELS "wallet")
    add_functional_test(wallet_multiwallet_legacy_rescan "wallet_multiwallet.py --legacy-wallet --rescan" COST 350 LABELS "wallet")
endif()

# Long-running base tests (~2-5 minutes) - COST 200-299
add_functional_test(mempool_limit "mempool_limit.py" COST 280)
add_functional_test(feature_fee_estimation "feature_fee_estimation.py" COST 270)
add_functional_test(p2p_node_network_limited_v1 "p2p_node_network_limited.py --v1transport" COST 260)
add_functional_test(p2p_node_network_limited_v2 "p2p_node_network_limited.py --v2transport" COST 260)

# Medium tests (~1-2 minutes) - COST 100-199
add_functional_test(mining_getblocktemplate_longpoll "mining_getblocktemplate_longpoll.py" COST 180)
add_functional_test(p2p_segwit "p2p_segwit.py" COST 170)
add_functional_test(feature_maxuploadtarget "feature_maxuploadtarget.py" COST 160)
add_functional_test(feature_assumeutxo "feature_assumeutxo.py" COST 150)
add_functional_test(mempool_updatefromblock "mempool_updatefromblock.py" COST 140)
add_functional_test(mempool_persist "mempool_persist.py" COST 130)

# Wallet tests (conditional on wallet support)
if(ENABLE_WALLET)
    # Medium wallet tests - COST 100-199
    add_functional_test(wallet_fundrawtransaction "wallet_fundrawtransaction.py" COST 120 LABELS "wallet")
    add_functional_test(wallet_bumpfee "wallet_bumpfee.py" COST 115 LABELS "wallet")
    add_functional_test(wallet_v3_txs "wallet_v3_txs.py" COST 110 LABELS "wallet")
    add_functional_test(wallet_backup "wallet_backup.py" COST 105 LABELS "wallet")
    add_functional_test(wallet_avoidreuse "wallet_avoidreuse.py" COST 100 LABELS "wallet")
    add_functional_test(wallet_address_types "wallet_address_types.py" COST 95 LABELS "wallet")
    add_functional_test(wallet_basic "wallet_basic.py" COST 90 LABELS "wallet")
    add_functional_test(wallet_multiwallet "wallet_multiwallet.py" COST 85 LABELS "wallet")
    add_functional_test(wallet_multiwallet_usecli "wallet_multiwallet.py --usecli" COST 85 LABELS "wallet")
    add_functional_test(wallet_groups "wallet_groups.py" COST 80 LABELS "wallet")
    add_functional_test(wallet_taproot "wallet_taproot.py" COST 75 LABELS "wallet")
    add_functional_test(wallet_listtransactions "wallet_listtransactions.py" COST 70 LABELS "wallet")
    add_functional_test(wallet_miniscript "wallet_miniscript.py" COST 65 LABELS "wallet")

    # Fast wallet tests - COST 50-99
    add_functional_test(wallet_signer "wallet_signer.py" COST 60 LABELS "wallet")
    add_functional_test(wallet_encryption "wallet_encryption.py" COST 55 LABELS "wallet")
    add_functional_test(wallet_hd "wallet_hd.py" COST 50 LABELS "wallet")
    add_functional_test(wallet_keypool_topup "wallet_keypool_topup.py" COST 45 LABELS "wallet")
    add_functional_test(wallet_balance "wallet_balance.py" COST 40 LABELS "wallet")
    add_functional_test(wallet_coinbase_category "wallet_coinbase_category.py" COST 35 LABELS "wallet")
    add_functional_test(wallet_importprunedfunds "wallet_importprunedfunds.py" COST 30 LABELS "wallet")
    add_functional_test(wallet_importdescriptors "wallet_importdescriptors.py" COST 25 LABELS "wallet")
    add_functional_test(wallet_listdescriptors "wallet_listdescriptors.py" COST 20 LABELS "wallet")
    add_functional_test(wallet_listreceivedby "wallet_listreceivedby.py" COST 15 LABELS "wallet")
    add_functional_test(tool_wallet "tool_wallet.py" COST 10 LABELS "wallet")
    add_functional_test(rpc_psbt "rpc_psbt.py" COST 5 LABELS "wallet")
    add_functional_test(feature_taproot "feature_taproot.py" COST 4 LABELS "wallet")
    add_functional_test(tool_signet_miner "tool_signet_miner.py" COST 3 LABELS "wallet")

    # SegWit tests require wallet functionality
    add_functional_test(feature_segwit_v2 "feature_segwit.py --v2transport" COST 90 LABELS "wallet")
    add_functional_test(feature_segwit_v1 "feature_segwit.py --v1transport" COST 90 LABELS "wallet")
endif()

# Fast base tests (~30s-1min) - COST 50-99
add_functional_test(p2p_tx_download "p2p_tx_download.py" COST 85)
add_functional_test(feature_abortnode "feature_abortnode.py" COST 80)
add_functional_test(feature_maxtipage "feature_maxtipage.py" COST 75)
add_functional_test(p2p_dns_seeds "p2p_dns_seeds.py" COST 70)
add_functional_test(p2p_blockfilters "p2p_blockfilters.py" COST 65)
add_functional_test(feature_assumevalid "feature_assumevalid.py" COST 60)
add_functional_test(feature_bip68_sequence "feature_bip68_sequence.py" COST 55)
add_functional_test(rpc_packages "rpc_packages.py" COST 50)

if(IS_LINUX)
    add_functional_test(rpc_bind_ipv4 "rpc_bind.py --ipv4" COST 45 LABELS "linux_only")
    add_functional_test(rpc_bind_ipv6 "rpc_bind.py --ipv6" COST 45 LABELS "linux_only")
    add_functional_test(rpc_bind_nonloopback "rpc_bind.py --nonloopback" COST 45 LABELS "linux_only")
endif()

# Very fast tests (<30s) - COST 1-49
add_functional_test(p2p_headers_sync_with_minchainwork "p2p_headers_sync_with_minchainwork.py" COST 40)
add_functional_test(p2p_feefilter "p2p_feefilter.py" COST 35)
add_functional_test(feature_csv_activation "feature_csv_activation.py" COST 30)
add_functional_test(p2p_sendheaders "p2p_sendheaders.py" COST 25)
add_functional_test(feature_config_args "feature_config_args.py" COST 20)
add_functional_test(p2p_invalid_messages "p2p_invalid_messages.py" COST 15)
add_functional_test(rpc_createmultisig "rpc_createmultisig.py" COST 10)
add_functional_test(p2p_timeouts_v1 "p2p_timeouts.py --v1transport" COST 8)
add_functional_test(p2p_timeouts_v2 "p2p_timeouts.py --v2transport" COST 8)
add_functional_test(rpc_signer "rpc_signer.py" COST 5)

if(ENABLE_USDT_TESTS AND IS_LINUX)
    add_functional_test(interface_usdt_coinselection "interface_usdt_coinselection.py" COST 60 LABELS "usdt;linux_only")
    add_functional_test(interface_usdt_mempool "interface_usdt_mempool.py" COST 55 LABELS "usdt;linux_only")
    add_functional_test(interface_usdt_net "interface_usdt_net.py" COST 50 LABELS "usdt;linux_only")
    add_functional_test(interface_usdt_utxocache "interface_usdt_utxocache.py" COST 45 LABELS "usdt;linux_only")
    add_functional_test(interface_usdt_validation "interface_usdt_validation.py" COST 40 LABELS "usdt;linux_only")
endif()

if(ENABLE_PREVIOUS_RELEASES_TESTS)
    add_functional_test(mempool_compatibility "mempool_compatibility.py" COST 50 LABELS "previous_releases")
    add_functional_test(feature_unsupported_utxo_db "feature_unsupported_utxo_db.py" COST 45 LABELS "previous_releases")
    add_functional_test(feature_coinstatsindex_compatibility "feature_coinstatsindex_compatibility.py" COST 40 LABELS "previous_releases")
    if(ENABLE_WALLET)
        add_functional_test(wallet_backwards_compatibility "wallet_backwards_compatibility.py" COST 35 LABELS "previous_releases;wallet")
    endif()
endif()

# Require specific routable addresses to be assigned to network interfaces
# These should be un-special-ed after https://github.com/bitcoin/bitcoin/pull/33362 is merged perhaps?
if(ENABLE_SPECIAL_NETWORK_TESTS)
    add_functional_test(feature_bind_port_externalip "feature_bind_port_externalip.py" COST 40 LABELS "special_network")
    add_functional_test(feature_bind_port_discover "feature_bind_port_discover.py" COST 40 LABELS "special_network")
endif()

# Coverage reporting test - runs after all functional tests complete
if(ENABLE_COVERAGE)
    add_test(
        NAME bitcoin.functional.coverage-report
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/functional/report_coverage.py ${CMAKE_CURRENT_BINARY_DIR}/coverage
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )

    set_tests_properties(bitcoin.functional.coverage-report PROPERTIES
        LABELS "functional;coverage"
        FIXTURES_REQUIRED "functional_cache"
        # This test should run after all other functional tests
        DEPENDS "bitcoin.functional.*"
    )
endif()
