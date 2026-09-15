// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <chainparams.h>
#include <chainparamsbase.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/license_info.h>
#include <common/system.h>
#include <compat/compat.h>
#include <key.h>
#include <logging.h>
#include <random.h>
#include <send.h>
#include <util/strencodings.h>
#include <util/threadinterrupt.h>
#include <util/translation.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef WIN32
#include <fcntl.h>
#include <io.h>
#endif

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

volatile std::sig_atomic_t g_interrupted{0};

void HandleSignal(int)
{
    g_interrupted = 1;
}

class Interrupt : public CThreadInterrupt
{
public:
    bool interrupted() const override { return g_interrupted != 0; }
};

bool InstallSignalHandlers()
{
#ifdef WIN32
    return std::signal(SIGINT, HandleSignal) != SIG_ERR && std::signal(SIGTERM, HandleSignal) != SIG_ERR;
#else
    struct sigaction action{};
    action.sa_handler = HandleSignal;
    sigemptyset(&action.sa_mask);
    // Do not restart stdin reads after cancellation.
    return sigaction(SIGINT, &action, nullptr) == 0 && sigaction(SIGTERM, &action, nullptr) == 0;
#endif
}

int Fail(const char* reason)
{
    std::fputs(reason, stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}

void SetupArgs(ArgsManager& args)
{
    SetupHelpOptions(args);
    args.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    constexpr auto VALUE_FLAGS = ArgsManager::DISALLOW_NEGATION | ArgsManager::DISALLOW_ELISION;
    args.AddArg("-proxy=<ip:port>", "Numeric loopback SOCKS5 endpoint with an explicit port (default: 127.0.0.1:9050). IPv6 must be bracketed. Uses Tor's default SOCKS-auth stream isolation.", VALUE_FLAGS, OptionsCategory::OPTIONS);
    args.AddArg("-chain=<chain>", "Select connection network (default: main). Allowed values: " LIST_CHAIN_NAMES ". A raw transaction has no chain tag.", VALUE_FLAGS, OptionsCategory::OPTIONS);
    args.AddArg("-timeout=<seconds>", "Positive total deadline per physical connection attempt (default: 60). One eligible v1 reconnect gets a fresh deadline.", VALUE_FLAGS, OptionsCategory::OPTIONS);
}

int Run(int argc, char* argv[])
{
    LogInstance().DisableLogging();
    SetupEnvironment();
    ArgsManager args;
    SetupArgs(args);
    // Parse only options. Keep all destination strings intact, including '='.
    int first_destination{1};
    while (first_destination < argc && argv[first_destination][0] == '-') {
        if (std::string{argv[first_destination]} == "-") return Fail("Invalid options.");
        ++first_destination;
    }
    std::string error;
    if (!args.ParseParameters(first_destination, argv, error)) return Fail("Invalid options.");
    if (HelpRequested(args) || args.GetBoolArg("-version", false)) {
        std::cout << CLIENT_NAME " bitcoin-broadcast utility version " << FormatFullVersion() << '\n';
        if (args.GetBoolArg("-version", false)) {
            std::cout << FormatParagraph(LicenseInfo());
        } else {
            std::cout << "\nUsage: bitcoin-broadcast [options] <onion-host[:port]> [<onion-host[:port]> ...]\n"
                         "\nRead one signed raw transaction in hex from stdin, optionally ending in LF or CRLF.\n"
                         "Connect through Tor to explicit v3 onions only. No node, wallet, configuration\n"
                         "file, or data directory is used. Uses Tor's default SOCKS-auth stream isolation.\n"
                         "No additional Tor configuration is normally required.\n"
                         "\nExit 0 only after complete tx and subsequent ping writes and a matching pong.\n"
                         "This proves neither mempool acceptance nor wider propagation. Success is silent.\n"
                         "After possible transaction disclosure, failure never tries another peer.\n"
                         "\nChecks are structural only. Signatures, inputs, fees, confirmation, and the\n"
                         "transaction's chain cannot be verified locally. Repeated invocations can\n"
                         "disclose the same transaction to more peers.\n\n"
                      << args.GetHelpMessage();
        }
        return EXIT_SUCCESS;
    }
    if (!SetupNetworking()) return Fail("Local setup failed.");
    try {
        SelectParams(args.GetChainType());
    } catch (const std::exception&) {
        return Fail("Invalid chain.");
    }
    std::vector<std::string> entries;
    for (int i = first_destination; i < argc; ++i)
        entries.emplace_back(argv[i]);
    const auto destinations = txsend::ParseDestinations(entries, Params().GetDefaultPort());
    if (!destinations) return Fail("Invalid onion destinations.");
    const auto proxy = txsend::ParseProxy(args.GetArg("-proxy", "127.0.0.1:9050"));
    if (!proxy) return Fail("Invalid proxy endpoint.");
    const auto timeout = txsend::ParseTimeout(args.GetArg("-timeout", "60"));
    if (!timeout) return Fail("Invalid timeout.");
    if (!InstallSignalHandlers()) return Fail("Local setup failed.");
    const Interrupt interrupt;
#ifdef WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return Fail("Local setup failed.");
#endif
    const auto tx = txsend::ReadTransaction(std::cin);
    if (interrupt) return Fail("Interrupted before transaction disclosure.");
    if (!tx) return Fail("Invalid transaction input.");
    RandomInit();
    const ECC_Context ecc_context;
    switch (txsend::SendTransaction(*tx, *destinations, *proxy, *timeout, interrupt)) {
    case txsend::SendResult::SUCCESS: return EXIT_SUCCESS;
    case txsend::SendResult::PROXY_ERROR: return Fail("Proxy setup failed before transaction disclosure.");
    case txsend::SendResult::LOCAL_ERROR: return Fail("Local setup failed before transaction disclosure.");
    case txsend::SendResult::INTERRUPTED: return Fail("Interrupted before transaction disclosure.");
    case txsend::SendResult::DISCLOSED: return Fail("Transaction information may have been disclosed; handoff not confirmed; no further peers tried.");
    case txsend::SendResult::PEER_ERROR: return Fail("Handoff failed before transaction disclosure.");
    }
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        return Run(argc, argv);
    } catch (const std::exception&) {
        // Never echo exception text from input, peers, or shared helpers.
        return Fail("Local setup failed; handoff not confirmed.");
    }
}
