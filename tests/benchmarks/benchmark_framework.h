#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rex::bench {

/// Result of a single benchmark
struct BenchmarkResult {
    std::string name;
    uint64_t iterations;
    double total_ms;
    double avg_ms;
    double min_ms;
    double max_ms;
    double ops_per_sec;
};

/// Simple benchmark runner
class Benchmark {
public:
    explicit Benchmark(std::string name, uint64_t iterations = 1000)
        : name_(std::move(name)), iterations_(iterations) {}

    /// Run the benchmark and return results
    BenchmarkResult run(std::function<void()> fn) {
        BenchmarkResult result{};
        result.name = name_;
        result.iterations = iterations_;
        result.min_ms = 1e9;
        result.max_ms = 0;

        // Warmup
        for (int i = 0; i < 10; ++i) fn();

        auto total_start = std::chrono::high_resolution_clock::now();

        for (uint64_t i = 0; i < iterations_; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            fn();
            auto end = std::chrono::high_resolution_clock::now();

            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            if (ms < result.min_ms) result.min_ms = ms;
            if (ms > result.max_ms) result.max_ms = ms;
        }

        auto total_end = std::chrono::high_resolution_clock::now();
        result.total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
        result.avg_ms = result.total_ms / static_cast<double>(iterations_);
        result.ops_per_sec = 1000.0 / result.avg_ms;

        return result;
    }

    /// Print benchmark result
    static void print_result(const BenchmarkResult& r) {
        printf("%-40s %8lu iters  avg=%.3f ms  min=%.3f ms  max=%.3f ms  (%.0f ops/s)\n",
               r.name.c_str(), r.iterations, r.avg_ms, r.min_ms, r.max_ms, r.ops_per_sec);
    }

private:
    std::string name_;
    uint64_t iterations_;
};

/// Benchmark suite runner
class BenchmarkSuite {
public:
    explicit BenchmarkSuite(std::string name) : name_(std::move(name)) {}

    void add(const std::string& bench_name, uint64_t iters, std::function<void()> fn) {
        benchmarks_.push_back({bench_name, iters, std::move(fn)});
    }

    void run_all() {
        printf("=== %s ===\n", name_.c_str());
        for (auto& [name, iters, fn] : benchmarks_) {
            Benchmark bench(name, iters);
            auto result = bench.run(fn);
            Benchmark::print_result(result);
            results_.push_back(result);
        }
        printf("\n");
    }

    const std::vector<BenchmarkResult>& results() const { return results_; }

private:
    struct BenchEntry {
        std::string name;
        uint64_t iterations;
        std::function<void()> fn;
    };

    std::string name_;
    std::vector<BenchEntry> benchmarks_;
    std::vector<BenchmarkResult> results_;
};

} // namespace rex::bench
