# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(DiscoverTests)

add_executable(bench_bitcoin
  bench_bitcoin.cpp
  bench.cpp
  nanobench.cpp
# Benchmarks:
  addrman.cpp
  base58.cpp
  bech32.cpp
  bip324_ecdh.cpp
  block_assemble.cpp
  blockencodings.cpp
  ccoins_caching.cpp
  chacha20.cpp
  checkblock.cpp
  checkblockindex.cpp
  checkqueue.cpp
  cluster_linearize.cpp
  connectblock.cpp
  crypto_hash.cpp
  descriptors.cpp
  disconnected_transactions.cpp
  duplicate_inputs.cpp
  ellswift.cpp
  examples.cpp
  gcs_filter.cpp
  hashpadding.cpp
  index_blockfilter.cpp
  load_external.cpp
  lockedpool.cpp
  logging.cpp
  mempool_ephemeral_spends.cpp
  mempool_eviction.cpp
  mempool_stress.cpp
  merkle_root.cpp
  obfuscation.cpp
  parse_hex.cpp
  peer_eviction.cpp
  poly1305.cpp
  pool.cpp
  prevector.cpp
  random.cpp
  readwriteblock.cpp
  rollingbloom.cpp
  rpc_blockchain.cpp
  rpc_mempool.cpp
  sign_transaction.cpp
  streams_findbyte.cpp
  strencodings.cpp
  txgraph.cpp
  txorphanage.cpp
  util_time.cpp
  verify_script.cpp
)

add_windows_application_manifest(bench_bitcoin)

include(TargetDataSources)
target_raw_data_sources(bench_bitcoin NAMESPACE benchmark::data
  data/block413567.raw
)

target_link_libraries(bench_bitcoin
  core_interface
  test_util
  bitcoin_node
  Boost::headers
)

if(ENABLE_WALLET)
  target_sources(bench_bitcoin
    PRIVATE
      coin_selection.cpp
      wallet_balance.cpp
      wallet_create.cpp
      wallet_create_tx.cpp
      wallet_loading.cpp
      wallet_ismine.cpp
      wallet_migration.cpp
  )
  target_link_libraries(bench_bitcoin bitcoin_wallet)
endif()

# By default, only run bench sanity check
add_test(NAME bench_sanity_check
  COMMAND bench_bitcoin -sanity-check
)
set_tests_properties(bench_sanity_check PROPERTIES
  LABELS bench
  ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/tsan:halt_on_error=1:second_deadlock_stack=1;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/lsan;UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/ubsan:print_stacktrace=1:halt_on_error=1:report_error_type=1;ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1"
)

# Optionally discover all individual benchmark tests
option(ENABLE_BENCH_TESTS "Enable individual benchmark tests (slow)" OFF)
if(ENABLE_BENCH_TESTS)
  discover_tests(bench_bitcoin
    DISCOVERY_ARGS        [[-list]]
    DISCOVERY_MATCH       [[.+]]
    TEST_NAME_REPLACEMENT [[bitcoin.bench.\0]]
    TEST_ARGS_REPLACEMENT [[-filter='^\0$']]
    PROPERTIES
      LABELS bench
      ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/tsan:halt_on_error=1:second_deadlock_stack=1;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/lsan;UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/test/sanitizer_suppressions/ubsan:print_stacktrace=1:halt_on_error=1:report_error_type=1;ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1"
  )
endif()

install_binary_component(bench_bitcoin INTERNAL)
