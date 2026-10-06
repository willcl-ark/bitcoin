// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <test/util/setup_common.h> // IWYU pragma: keep
#include <util/check.h>
#include <util/fs.h>
#include <util/time.h>
#include <univalue.h>

#include <compare>
#include <cstddef>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Retrieves the available test setup command line arguments that may be used
 * in the benchmark. They will be used only if the benchmark utilizes a
 * 'BasicTestingSetup' or any child of it.
 */
static std::function<std::vector<const char*>()> g_bench_command_line_args{};
const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS = []() {
    return g_bench_command_line_args();
};

/**
 * Retrieve the name of the currently in-use benchmark.
 * This is applicable only to benchmarks that utilize the unit test
 * framework context setup (e.g. ones using 'MakeNoLogFileContext<TestingSetup>()').
 * It places the datadir of each benchmark run within their respective benchmark name.
 */
static std::string g_running_benchmark_name;
const std::function<std::string()> G_TEST_GET_FULL_NAME = []() {
    return g_running_benchmark_name;
};

namespace {

struct ProfileResult {
    std::string name;
    double function_seconds;
    double nanobench_seconds;
    double nanobench_iterations;
    size_t nanobench_epochs;
};

void WriteProfileResults(const std::vector<ProfileResult>& profile_results, const fs::path& profile_path)
{
    if (profile_results.empty() || profile_path.empty()) return;
    if (!profile_path.parent_path().empty()) {
        fs::create_directories(profile_path.parent_path());
    }
    UniValue timings{UniValue::VARR};
    for (const auto& result : profile_results) {
        UniValue timing{UniValue::VOBJ};
        timing.pushKV("name", "bench." + result.name);
        timing.pushKV("calls", 1);
        timing.pushKV("seconds", result.function_seconds);
        timing.pushKV("nanobench_seconds", result.nanobench_seconds);
        timing.pushKV("nanobench_iterations", result.nanobench_iterations);
        timing.pushKV("nanobench_epochs", result.nanobench_epochs);
        timings.push_back(std::move(timing));
    }
    UniValue metadata{UniValue::VOBJ};
    metadata.pushKV("profile_type", "bench");
    UniValue profile{UniValue::VOBJ};
    profile.pushKV("version", 1);
    profile.pushKV("metadata", std::move(metadata));
    profile.pushKV("timings", std::move(timings));
    std::ofstream out{profile_path.std_path()};
    out << profile.write(2) << '\n';
}

void GenerateTemplateResults(const std::vector<ankerl::nanobench::Result>& benchmarkResults, const fs::path& file, const char* tpl)
{
    if (benchmarkResults.empty() || file.empty()) {
        // nothing to write, bail out
        return;
    }
    std::ofstream fout{file.std_path()};
    if (fout.is_open()) {
        ankerl::nanobench::render(tpl, benchmarkResults, fout);
        std::cout << "Created " << file << std::endl;
    } else {
        std::cout << "Could not write to file " << file << std::endl;
    }
}

} // namespace

namespace benchmark {

BenchRunner::BenchmarkMap& BenchRunner::benchmarks()
{
    static BenchmarkMap benchmarks_map;
    return benchmarks_map;
}

BenchRunner::BenchRunner(std::string_view name, BenchFunction func)
{
    Assert(benchmarks().try_emplace(std::string{name}, std::move(func)).second);
}

void BenchRunner::RunAll(const Args& args)
{
    std::regex reFilter(args.regex_filter);
    std::smatch baseMatch;

    if (args.sanity_check) {
        std::cout << "Running with -sanity-check option, output is being suppressed as benchmark results will be useless." << std::endl;
    }

    // Load inner test setup args
    g_bench_command_line_args = [&args]() {
        std::vector<const char*> ret;
        ret.reserve(args.setup_args.size());
        for (const auto& arg : args.setup_args) ret.emplace_back(arg.c_str());
        return ret;
    };

    std::vector<ankerl::nanobench::Result> benchmarkResults;
    std::vector<ProfileResult> profile_results;
    for (const auto& [name, func] : benchmarks()) {

        if (!std::regex_match(name, baseMatch, reFilter)) {
            continue;
        }

        if (args.is_list_only) {
            std::cout << name << std::endl;
            continue;
        }

        Bench bench;
        if (args.sanity_check) {
            bench.epochs(1).epochIterations(1);
            bench.output(nullptr);
        }
        bench.name(name);
        g_running_benchmark_name = name;
        if (args.min_time > 0ms) {
            // convert to nanos before dividing to reduce rounding errors
            std::chrono::nanoseconds min_time_ns = args.min_time;
            bench.minEpochTime(min_time_ns / bench.epochs());
        }

        const auto result_start{bench.results().size()};
        const auto start{args.profile_output.empty() ? SteadyClock::time_point{} : SteadyClock::now()};
        if (args.asymptote.empty()) {
            func(bench);
        } else {
            for (auto n : args.asymptote) {
                bench.complexityN(n);
                func(bench);
            }
            std::cout << bench.complexityBigO() << std::endl;
        }

        if (!bench.results().empty()) {
            benchmarkResults.push_back(bench.results().back());
        }
        if (!args.profile_output.empty() && bench.results().size() > result_start) {
            const auto function_seconds{std::chrono::duration<double>(SteadyClock::now() - start).count()};
            double nanobench_seconds{0.0};
            double nanobench_iterations{0.0};
            size_t nanobench_epochs{0};
            for (size_t i{result_start}; i < bench.results().size(); ++i) {
                const auto& result{bench.results()[i]};
                nanobench_seconds += result.sumProduct(ankerl::nanobench::Result::Measure::iterations, ankerl::nanobench::Result::Measure::elapsed);
                nanobench_iterations += result.sum(ankerl::nanobench::Result::Measure::iterations);
                nanobench_epochs += result.size();
            }
            profile_results.push_back({
                .name = name,
                .function_seconds = function_seconds,
                .nanobench_seconds = nanobench_seconds,
                .nanobench_iterations = nanobench_iterations,
                .nanobench_epochs = nanobench_epochs,
            });
        }
    }

    GenerateTemplateResults(benchmarkResults, args.output_csv, "# Benchmark, evals, iterations, total, min, max, median\n"
                                                               "{{#result}}{{name}}, {{epochs}}, {{average(iterations)}}, {{sumProduct(iterations, elapsed)}}, {{minimum(elapsed)}}, {{maximum(elapsed)}}, {{median(elapsed)}}\n"
                                                               "{{/result}}");
    GenerateTemplateResults(benchmarkResults, args.output_json, ankerl::nanobench::templates::json());
    WriteProfileResults(profile_results, args.profile_output);
}

} // namespace benchmark
