#include "ThreadCache.h"
#include "Common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace memoryPool;

class Timer {
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    double elapsedMs() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count() / 1000.0;
    }
private:
    std::chrono::steady_clock::time_point start_;
};

struct Block {
    void* ptr{nullptr};
    size_t size{0};
    uint32_t tag{0};
};

static void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static void* poolAlloc(size_t size) {
    return ThreadCache::getInstance()->allocate(size);
}

static void poolFree(void* ptr, size_t size) {
    ThreadCache::getInstance()->deallocate(ptr, size);
}

static unsigned char makeByte(size_t size, uint32_t tag, size_t i) {
    return static_cast<unsigned char>(((size * 131u) ^ (tag * 17u) ^ (i * 31u)) & 0xFFu);
}

static void fillPattern(void* ptr, size_t size, uint32_t tag) {
    auto* p = static_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i) p[i] = makeByte(size, tag, i);
}

static bool checkPattern(const void* ptr, size_t size, uint32_t tag) {
    const auto* p = static_cast<const unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        if (p[i] != makeByte(size, tag, i)) return false;
    }
    return true;
}

static std::vector<size_t> boundarySizes() {
    return {
        1, 2, 7, 8, 9,
        15, 16, 17,
        31, 32, 33,
        63, 64, 65,
        127, 128, 129,
        255, 256, 257,
        511, 512, 513,
        1023, 1024, 1025,
        2047, 2048, 2049,
        4095, 4096, 4097,
        MAX_BYTES - 1, MAX_BYTES, MAX_BYTES + 1
    };
}

class CorrectnessTest {
public:
    static void runAll() {
        std::cout << "================ Correctness Tests ================\n";
        runOne("Basic sizes", testBasicSizes);
        runOne("Boundary sizes", testBoundarySizes);
        runOne("Reuse behavior", testReuseBehavior);
        runOne("Multi-thread smoke", testMultiThreadSmoke);
        std::cout << "[ALL PASS] correctness tests passed.\n\n";
    }

private:
    template <class Func>
    static void runOne(const std::string& name, Func func) {
        try {
            func();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << name << " -> " << e.what() << '\n';
            throw;
        }
    }

    static void testBasicSizes() {
        std::vector<size_t> sizes = {1, 8, 16, 24, 32, 64, 128, 256, 512, 1024, 4096};
        uint32_t tag = 1;
        for (size_t size : sizes) {
            void* ptr = poolAlloc(size);
            expect(ptr != nullptr, "allocate returned nullptr");
            fillPattern(ptr, size, tag);
            expect(checkPattern(ptr, size, tag), "pattern check failed");
            poolFree(ptr, size);
            ++tag;
        }
    }

    static void testBoundarySizes() {
        std::vector<Block> blocks;
        uint32_t tag = 100;
        for (size_t size : boundarySizes()) {
            void* ptr = poolAlloc(size);
            expect(ptr != nullptr, "boundary allocate returned nullptr, size=" + std::to_string(size));
            fillPattern(ptr, size, tag);
            blocks.push_back({ptr, size, tag});
            ++tag;
        }

        for (const auto& block : blocks) {
            expect(checkPattern(block.ptr, block.size, block.tag),
                   "boundary pattern corrupted, size=" + std::to_string(block.size));
        }

        for (const auto& block : blocks) {
            poolFree(block.ptr, block.size);
        }
    }

    static void testReuseBehavior() {
        std::vector<Block> firstRound;
        for (uint32_t tag = 1; tag <= 200; ++tag) {
            size_t size = ((tag % 16) + 1) * 8;
            void* ptr = poolAlloc(size);
            expect(ptr != nullptr, "reuse test allocate returned nullptr");
            fillPattern(ptr, size, tag);
            firstRound.push_back({ptr, size, tag});
        }

        for (const auto& block : firstRound) {
            expect(checkPattern(block.ptr, block.size, block.tag), "reuse test pattern corrupted before free");
            poolFree(block.ptr, block.size);
        }

        for (uint32_t tag = 1001; tag <= 1200; ++tag) {
            size_t size = (((tag - 1000) % 16) + 1) * 8;
            void* ptr = poolAlloc(size);
            expect(ptr != nullptr, "reuse round 2 allocate returned nullptr");
            fillPattern(ptr, size, tag);
            expect(checkPattern(ptr, size, tag), "reuse round 2 pattern corrupted");
            poolFree(ptr, size);
        }
    }

    static void testMultiThreadSmoke() {
        constexpr size_t THREADS = 4;
        constexpr size_t OPS_PER_THREAD = 2000;
        std::atomic<bool> ok{true};

        auto worker = [&](size_t tid) {
            std::mt19937 rng(static_cast<uint32_t>(20260422 + tid * 97));
            std::vector<Block> live;
            live.reserve(256);

            for (size_t i = 0; i < OPS_PER_THREAD && ok.load(); ++i) {
                bool doAlloc = live.empty() || (live.size() < 128 && (rng() % 100) < 70);
                if (doAlloc) {
                    size_t size = (static_cast<size_t>(rng() % 32) + 1) * 8;
                    uint32_t tag = static_cast<uint32_t>(tid * 100000 + i + 1);
                    void* ptr = poolAlloc(size);
                    if (!ptr) {
                        ok.store(false);
                        return;
                    }
                    fillPattern(ptr, size, tag);
                    live.push_back({ptr, size, tag});
                } else {
                    size_t index = static_cast<size_t>(rng() % live.size());
                    Block block = live[index];
                    if (!checkPattern(block.ptr, block.size, block.tag)) {
                        ok.store(false);
                        return;
                    }
                    poolFree(block.ptr, block.size);
                    live[index] = live.back();
                    live.pop_back();
                }
            }

            for (const auto& block : live) {
                if (!checkPattern(block.ptr, block.size, block.tag)) {
                    ok.store(false);
                    return;
                }
                poolFree(block.ptr, block.size);
            }
        };

        std::vector<std::thread> threads;
        for (size_t i = 0; i < THREADS; ++i) threads.emplace_back(worker, i);
        for (auto& t : threads) t.join();
        expect(ok.load(), "multi-thread smoke test failed");
    }
};

struct Workload {
    std::vector<size_t> sizes;
    std::vector<unsigned char> shouldRelease;
    std::vector<uint32_t> tickets;
};

class PerformanceTest {
public:
    static void runAll() {
        std::cout << "================ Performance Tests ================\n";
        warmup();
        Workload small = makeSmallWorkload(10000, 20260422);
        Workload mixed = makeMixedWorkload(15000, 20260423);
        std::vector<Workload> multi = makeMultiThreadWorkloads(4, 5000, 20260424);
        printHeader();
        runSingleThreadRow("Small fixed sizes", small, 3);
        runSingleThreadRow("Mixed sizes", mixed, 3);
        runMultiThreadRow("Multi-thread", multi, 2);
        std::cout << '\n';
    }

private:
    static void warmup() {
        const std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1024};
        for (int i = 0; i < 3000; ++i) {
            size_t size = sizes[static_cast<size_t>(i) % sizes.size()];
            void* ptr = poolAlloc(size);
            poolFree(ptr, size);
        }
        for (int i = 0; i < 3000; ++i) {
            size_t size = sizes[static_cast<size_t>(i) % sizes.size()];
            char* ptr = new char[size];
            delete[] ptr;
        }
    }

    static Workload makeSmallWorkload(size_t ops, uint32_t seed) {
        Workload wl;
        wl.sizes.reserve(ops);
        wl.shouldRelease.reserve(ops);
        wl.tickets.reserve(ops);
        const std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256};
        std::mt19937 rng(seed);
        for (size_t i = 0; i < ops; ++i) {
            wl.sizes.push_back(sizes[static_cast<size_t>(rng() % sizes.size())]);
            wl.shouldRelease.push_back((i % 4 == 0) ? 1 : 0);
            wl.tickets.push_back(rng());
        }
        return wl;
    }

    static Workload makeMixedWorkload(size_t ops, uint32_t seed) {
        Workload wl;
        wl.sizes.reserve(ops);
        wl.shouldRelease.reserve(ops);
        wl.tickets.reserve(ops);
        const std::vector<size_t> sizes = {
            8, 16, 32, 64, 128, 256, 384, 512,
            1024, 2048, 4096, 8192
        };
        std::mt19937 rng(seed);
        for (size_t i = 0; i < ops; ++i) {
            wl.sizes.push_back(sizes[static_cast<size_t>(rng() % sizes.size())]);
            wl.shouldRelease.push_back((i % 5 == 0) ? 1 : 0);
            wl.tickets.push_back(rng());
        }
        return wl;
    }

    static std::vector<Workload> makeMultiThreadWorkloads(size_t threads, size_t opsPerThread, uint32_t seed) {
        std::vector<Workload> result;
        result.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            result.push_back(makeMixedWorkload(opsPerThread, seed + static_cast<uint32_t>(i * 101)));
        }
        return result;
    }

    static void executePool(const Workload& wl) {
        std::vector<std::pair<void*, size_t>> live;
        live.reserve(wl.sizes.size() / 2 + 1024);
        for (size_t i = 0; i < wl.sizes.size(); ++i) {
            size_t size = wl.sizes[i];
            void* ptr = poolAlloc(size);
            auto* p = static_cast<unsigned char*>(ptr);
            p[0] = static_cast<unsigned char>(size & 0xFFu);
            p[size - 1] = static_cast<unsigned char>((size * 3u) & 0xFFu);
            live.push_back({ptr, size});
            if (wl.shouldRelease[i] && !live.empty()) {
                size_t index = static_cast<size_t>(wl.tickets[i]) % live.size();
                poolFree(live[index].first, live[index].second);
                live[index] = live.back();
                live.pop_back();
            }
        }
        for (const auto& block : live) poolFree(block.first, block.second);
    }

    static void executeSystem(const Workload& wl) {
        std::vector<std::pair<void*, size_t>> live;
        live.reserve(wl.sizes.size() / 2 + 1024);
        for (size_t i = 0; i < wl.sizes.size(); ++i) {
            size_t size = wl.sizes[i];
            char* ptr = new char[size];
            ptr[0] = static_cast<unsigned char>(size & 0xFFu);
            ptr[size - 1] = static_cast<unsigned char>((size * 3u) & 0xFFu);
            live.push_back({ptr, size});
            if (wl.shouldRelease[i] && !live.empty()) {
                size_t index = static_cast<size_t>(wl.tickets[i]) % live.size();
                delete[] static_cast<char*>(live[index].first);
                live[index] = live.back();
                live.pop_back();
            }
        }
        for (const auto& block : live) delete[] static_cast<char*>(block.first);
    }

    template <class Func>
    static double averageMs(size_t rounds, Func func) {
        double total = 0.0;
        for (size_t i = 0; i < rounds; ++i) {
            Timer t;
            func();
            total += t.elapsedMs();
        }
        return total / static_cast<double>(rounds);
    }

    static void printHeader() {
        std::cout << std::left << std::setw(20) << "Benchmark"
                  << std::right << std::setw(18) << "MemoryPool(ms)"
                  << std::setw(18) << "new/delete(ms)"
                  << std::setw(14) << "Speedup" << '\n';
        std::cout << std::string(70, '-') << '\n';
    }

    static void printRow(const std::string& name, double poolMs, double sysMs) {
        double speedup = poolMs > 0.0 ? sysMs / poolMs : 0.0;
        std::cout << std::left << std::setw(20) << name
                  << std::right << std::setw(18) << std::fixed << std::setprecision(3) << poolMs
                  << std::setw(18) << std::fixed << std::setprecision(3) << sysMs
                  << std::setw(14) << std::fixed << std::setprecision(2) << speedup << '\n';
    }

    static void runSingleThreadRow(const std::string& name, const Workload& wl, size_t rounds) {
        double poolMs = averageMs(rounds, [&]() { executePool(wl); });
        double sysMs = averageMs(rounds, [&]() { executeSystem(wl); });
        printRow(name, poolMs, sysMs);
    }

    static void runMultiThreadRow(const std::string& name, const std::vector<Workload>& workloads, size_t rounds) {
        double poolMs = averageMs(rounds, [&]() {
            std::vector<std::thread> threads;
            for (const auto& wl : workloads) threads.emplace_back([&wl]() { executePool(wl); });
            for (auto& t : threads) t.join();
        });
        double sysMs = averageMs(rounds, [&]() {
            std::vector<std::thread> threads;
            for (const auto& wl : workloads) threads.emplace_back([&wl]() { executeSystem(wl); });
            for (auto& t : threads) t.join();
        });
        printRow(name, poolMs, sysMs);
    }
};

int main() {
    try {
        std::cout << "Starting memory pool tests...\n\n";
        CorrectnessTest::runAll();
        PerformanceTest::runAll();
        std::cout << "All tests finished successfully.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test stopped: " << e.what() << '\n';
        return 1;
    }
}