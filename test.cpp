/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#include <cstddef>

// bbsl now allocates through its own arena slab, invisible to the operator-new seam, so it
// reports its own exact usage via a traversal-based memoryUsage(); compiled in by defining
// BBSL_MEMORY_STATS before the header
#define BBSL_MEMORY_STATS
#include "./src/bbsl.hpp"
#include <cmath>
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <map>
#include <random>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <cstdlib>
#include <new>

constexpr auto testCount = 1'000'000;
using namespace bbsl;

// Zipf distribution generator for realistic access patterns
class ZipfGenerator {
private:
    std::mt19937_64 rng;
    double alpha;
    std::vector<double> probabilities;

public:
    ZipfGenerator(uint64_t seed, uint64_t n, double alpha = 0.8)
        : rng(seed), alpha(alpha) {
        probabilities.resize(n);
        double sum = 0;
        for (uint64_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(i, alpha);
        }
        double cumulative = 0;
        for (uint64_t i = 0; i < n; ++i) {
            cumulative += 1.0 / std::pow(i + 1, alpha) / sum;
            probabilities[i] = cumulative;
        }
    }

    uint64_t next() {
        double r = std::uniform_real_distribution<double>(0, 1)(rng);
        auto it = std::lower_bound(probabilities.begin(), probabilities.end(), r);
        return std::distance(probabilities.begin(), it);
    }
};

// pre-generate Zipf indices so the generator cost stays outside the timed region
std::vector<uint64_t> generateZipfIndices(ZipfGenerator& zipf, uint64_t count) {
    std::vector<uint64_t> indices;
    indices.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        indices.push_back(zipf.next());
    }
    return indices;
}

void test1() {
    BitmappedBlockSkipList<uint64_t, double> skiplist(std::nan(""));

    // test insert
    for (uint64_t i = 0; i < 10; ++i) {
        skiplist[i] = i * 1.5;
    }

    // test search
    for (uint64_t i = 0; i < 10; ++i) {
        assert(skiplist.has(i));
        double value = skiplist[i];
        assert(value == i * 1.5);
    }

    // find not exist
    assert(!skiplist.has(100));
    double notfound = skiplist[100];
    assert(std::isnan(notfound));

    // test delete
    skiplist.erase(5);
    assert(!skiplist.has(5));
    double deleted = skiplist[5];
    assert(std::isnan(deleted));

    // range test
    skiplist[31] = 99.9;
    assert(skiplist.has(31));
    double edge = skiplist[31];
    assert(edge == 99.9);

    std::cout << "test1 passed!" << std::endl;
}

void test2() {
    // Test large range insertion and sparsity
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);
    for (uint64_t i = 0; i < 1000; i += 100) {
        skiplist[i] = static_cast<int>(i * 2);
    }
    for (uint64_t i = 0; i < 1000; ++i) {
        if (i % 100 == 0) {
            assert(skiplist.has(i));
            int value = skiplist[i];
            assert(value == static_cast<int>(i * 2));
        }
        else {
            assert(!skiplist.has(i));
            int value = skiplist[i];
            assert(value == -1);
        }
    }
    std::cout << "test2 passed!" << std::endl;
}

void test3() {
    // Test deletion and reinsertion
    BitmappedBlockSkipList<uint64_t, int> skiplist(-999);
    skiplist[10] = 42;
    assert(skiplist.has(10));
    int value = skiplist[10];
    assert(value == 42);
    skiplist.erase(10);
    assert(!skiplist.has(10));
    value = skiplist[10];
    assert(value == -999);
    skiplist[10] = 100;
    assert(skiplist.has(10));
    value = skiplist[10];
    assert(value == 100);
    std::cout << "test3 passed!" << std::endl;
}

void test4() {
    // Test boundary conditions
    BitmappedBlockSkipList<uint64_t, double> skiplist(std::nan(""));
    assert(!skiplist.has(0));
    double value = skiplist[0];
    assert(std::isnan(value));
    skiplist[0] = 3.14;
    assert(skiplist.has(0));
    value = skiplist[0];
    assert(value == 3.14);
    skiplist[UINT64_MAX] = 2.71;
    assert(skiplist.has(UINT64_MAX));
    value = skiplist[UINT64_MAX];
    assert(value == 2.71);
    std::cout << "test4 passed!" << std::endl;
}

// ============= Original Performance Tests =============

void test_performance_stdmap(uint64_t seed) {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;

    auto start_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        m[i] = static_cast<int>(i);
    }
    auto end_insert = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> insert_duration = end_insert - start_insert;
    std::cout << "[std::map] Insert " << N << " elements took: " << insert_duration.count() << " seconds" << std::endl;

    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        auto it = m.find(i);
        int value = (it != m.end()) ? it->second : -1;
        sum += value;
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> query_duration = end_query - start_query;
    std::cout << "[std::map] Query " << N << " elements took: " << query_duration.count() << " seconds" << std::endl;
    std::cout << "[std::map] Query sum: " << sum << std::endl;

    // Random access test (using fast LCG)
    auto start_random_query = std::chrono::high_resolution_clock::now();
    int random_sum = 0;
    const uint64_t a = 6364136223846793005ULL;
    const uint64_t c = 1;
    uint64_t seed_tmp = seed;
    for (uint64_t i = 0; i < N; ++i) {
        seed_tmp = seed_tmp * a + c;
        uint64_t idx = seed_tmp % N;
        auto it = m.find(idx);
        int value = (it != m.end()) ? it->second : -1;
        random_sum += value;
    }
    auto end_random_query = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> random_query_duration = end_random_query - start_random_query;
    std::cout << "[std::map] Random query " << N << " times took: " << random_query_duration.count() << " seconds" << std::endl;
    std::cout << "[std::map] Random query sum: " << random_sum << std::endl;
}

void test_performance_bsl(uint64_t seed) {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    auto start_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }
    auto end_insert = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> insert_duration = end_insert - start_insert;
    std::cout << "[bsl] Insert " << N << " elements took: " << insert_duration.count() << " seconds" << std::endl;

    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        int value = skiplist[i];
        sum += value;
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> query_duration = end_query - start_query;
    std::cout << "[bsl] Query " << N << " elements took: " << query_duration.count() << " seconds" << std::endl;
    std::cout << "[bsl] Query sum: " << sum << std::endl;

    std::cout << "[bsl] level " << skiplist.getLevel() << std::endl;

    auto start_random_query = std::chrono::high_resolution_clock::now();
    int random_sum = 0;
    const uint64_t a = 6364136223846793005ULL;
    const uint64_t c = 1;
    uint64_t seed_tmp = seed;
    for (uint64_t i = 0; i < N; ++i) {
        seed_tmp = seed_tmp * a + c;
        uint64_t idx = seed_tmp % N;
        int value = skiplist[idx];
        random_sum += value;
    }
    auto end_random_query = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> random_query_duration = end_random_query - start_random_query;
    std::cout << "[bsl] Random query " << N << " times took: " << random_query_duration.count() << " seconds" << std::endl;
    std::cout << "[bsl] Random query sum: " << random_sum << std::endl;
}

void test_performance_random_stdmap(uint64_t seedA, uint64_t seedB) {
    const uint64_t N = testCount;
    const uint64_t deleteCount = static_cast<uint64_t>(N * 0.2);
    std::map<uint64_t, int> m;

    auto start_map_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) m[i] = static_cast<int>(i);
    auto end_map_insert = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Insert " << N << " : " << (end_map_insert - start_map_insert).count() / 1e9 << "s\n";

    uint64_t seed = seedA, a = 6364136223846793005ULL, c = 1;
    auto start_map_delete = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < deleteCount; ++i) {
        seed = seed * a + c;
        m.erase(seed);
    }
    auto end_map_delete = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Delete " << deleteCount << " : " << (end_map_delete - start_map_delete).count() / 1e9 << "s\n";

    seed = seedB;
    auto start_map_write_delete = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        seed = seed * a + c;
        uint64_t idx = seed % N;
        if (i % 2 == 0) m[idx] = static_cast<int>(i);
        else m.erase(idx);
    }
    auto end_map_write_delete = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Write/Delete " << N << " : " << (end_map_write_delete - start_map_write_delete).count() / 1e9 << "s\n";
}

void test_performance_random_bsl(uint64_t seedA, uint64_t seedB) {
    const uint64_t N = testCount;
    const uint64_t deleteCount = static_cast<uint64_t>(N * 0.2);
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    auto start_bsl_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) skiplist[i] = static_cast<int>(i);
    auto end_bsl_insert = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Insert " << N << " : " << (end_bsl_insert - start_bsl_insert).count() / 1e9 << "s\n";

    uint64_t seed = seedA, a = 6364136223846793005ULL, c = 1;
    auto start_bsl_delete = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < deleteCount; ++i) {
        seed = seed * a + c;
        skiplist.erase(seed);
    }
    auto end_bsl_delete = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Delete " << deleteCount << " : " << (end_bsl_delete - start_bsl_delete).count() / 1e9 << "s\n";

    seed = seedB;
    auto start_bsl_write_delete = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        seed = seed * a + c;
        uint64_t idx = seed % N;
        if (i % 2 == 0) skiplist[idx] = static_cast<int>(i);
        else skiplist.erase(idx);
    }
    auto end_bsl_write_delete = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Write/Delete " << N << " : " << (end_bsl_write_delete - start_bsl_write_delete).count() / 1e9 << "s\n";
}

// ============= New: Sequential Access Performance Tests =============

void test_performance_sequential_stdmap(uint64_t seed) {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;

    // Sequential insertion
    auto start_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        m[i] = static_cast<int>(i);
    }
    auto end_insert = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Sequential Insert " << N << " : "
        << (end_insert - start_insert).count() / 1e9 << "s\n";

    // Sequential query
    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum += m[i];
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Sequential Query " << N << " : "
        << (end_query - start_query).count() / 1e9 << "s\n";
    std::cout << "[std::map] Sum: " << sum << std::endl;
}

void test_performance_sequential_bsl(uint64_t seed) {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    // Sequential insertion
    auto start_insert = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }
    auto end_insert = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Sequential Insert " << N << " : "
        << (end_insert - start_insert).count() / 1e9 << "s\n";

    // Sequential query
    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum += skiplist[i];
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Sequential Query " << N << " : "
        << (end_query - start_query).count() / 1e9 << "s\n";
    std::cout << "[bsl] Sum: " << sum << std::endl;
    std::cout << "[bsl] level " << skiplist.getLevel() << std::endl;
}

// ============= New: Zipf Distribution (Realistic Scenario) Tests =============

void test_performance_zipf_stdmap(uint64_t seed) {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;
    ZipfGenerator zipf(seed, N, 0.8);

    // Pre-insert all data
    for (uint64_t i = 0; i < N; ++i) {
        m[i] = static_cast<int>(i);
    }

    // Zipf distribution query (indices pre-generated outside the timed region)
    std::vector<uint64_t> indices = generateZipfIndices(zipf, N);
    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum += m[indices[i]];
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Zipf Query (α=0.8) " << N << " : "
        << (end_query - start_query).count() / 1e9 << "s\n";
    std::cout << "[std::map] Sum: " << sum << std::endl;

    // Zipf distribution mixed operations (80% query, 20% update), indices pre-generated
    ZipfGenerator zipf2(seed + 1, N, 0.8);
    std::vector<uint64_t> indices2 = generateZipfIndices(zipf2, N);
    auto start_mixed = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        uint64_t idx = indices2[i];
        if (i % 5 == 0) {  // 20% write
            m[idx] = static_cast<int>(i);
        }
        else {  // 80% query
            sum += m[idx];
        }
    }
    auto end_mixed = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Zipf Mixed (80/20) " << N << " : "
        << (end_mixed - start_mixed).count() / 1e9 << "s\n";
}

void test_performance_zipf_bsl(uint64_t seed) {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);
    ZipfGenerator zipf(seed, N, 0.8);

    // Pre-insert all data
    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }

    // Zipf distribution query (indices pre-generated outside the timed region)
    std::vector<uint64_t> indices = generateZipfIndices(zipf, N);
    auto start_query = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum += skiplist[indices[i]];
    }
    auto end_query = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Zipf Query (α=0.8) " << N << " : "
        << (end_query - start_query).count() / 1e9 << "s\n";
    std::cout << "[bsl] Sum: " << sum << std::endl;
    std::cout << "[bsl] level " << skiplist.getLevel() << std::endl;

    // Zipf distribution mixed operations (80% query, 20% update), indices pre-generated
    ZipfGenerator zipf2(seed + 1, N, 0.8);
    std::vector<uint64_t> indices2 = generateZipfIndices(zipf2, N);
    auto start_mixed = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
        uint64_t idx = indices2[i];
        if (i % 5 == 0) {  // 20% write
            skiplist[idx] = static_cast<int>(i);
        }
        else {  // 80% query
            sum += skiplist[idx];
        }
    }
    auto end_mixed = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Zipf Mixed (80/20) " << N << " : "
        << (end_mixed - start_mixed).count() / 1e9 << "s\n";
}

// ============= New: Range Query Performance Tests =============

void test_performance_range_stdmap() {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;

    for (uint64_t i = 0; i < N; ++i) {
        m[i] = static_cast<int>(i);
    }

    auto start_range = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        uint64_t start = i * 1000;
        uint64_t end = start + 500;
        auto it = m.lower_bound(start);
        while (it != m.end() && it->first < end) {
            sum += it->second;
            ++it;
        }
    }
    auto end_range = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Range queries (500 elements x 1000) : "
        << (end_range - start_range).count() / 1e9 << "s\n";
    std::cout << "[std::map] Range sum: " << sum << std::endl;
}

void test_performance_range_bsl() {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }

    auto start_range = std::chrono::high_resolution_clock::now();
    int sum = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        uint64_t start = i * 1000;
        uint64_t end = start + 500;
        for (uint64_t j = start; j < end; ++j) {
            sum += skiplist[j];
        }
    }
    auto end_range = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Range queries (500 elements x 1000) : "
        << (end_range - start_range).count() / 1e9 << "s\n";
    std::cout << "[bsl] Range sum: " << sum << std::endl;
}

void test_performance_range_scan_bsl() {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }

    // one lowerBound per window, then walk the level-0 chain - the real range-scan pattern
    auto start_range = std::chrono::high_resolution_clock::now();
    long long sum = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        uint64_t start = i * 1000;
        uint64_t end = start + 500;
        for (auto it = skiplist.lowerBound(start); it != skiplist.end() && it.key() < end; ++it) {
            sum += *it;
        }
    }
    auto end_range = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Range scans (500 elements x 1000) : "
        << (end_range - start_range).count() / 1e9 << "s\n";
    std::cout << "[bsl] Range scan sum: " << sum << std::endl;
}

// ============= New: Batch Operation Performance Tests =============

void test_performance_batch_stdmap() {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;

    // Batch insert (1000 per batch)
    auto start_batch = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; i += 1000) {
        for (uint64_t j = 0; j < 1000; ++j) {
            m[i + j] = static_cast<int>(i + j);
        }
    }
    auto end_batch = std::chrono::high_resolution_clock::now();
    std::cout << "[std::map] Batch insert (1000/batch) : "
        << (end_batch - start_batch).count() / 1e9 << "s\n";
}

void test_performance_batch_bsl() {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    // Batch insert (1000 per batch)
    auto start_batch = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < N; i += 1000) {
        for (uint64_t j = 0; j < 1000; ++j) {
            skiplist[i + j] = static_cast<int>(i + j);
        }
    }
    auto end_batch = std::chrono::high_resolution_clock::now();
    std::cout << "[bsl] Batch insert (1000/batch) : "
        << (end_batch - start_batch).count() / 1e9 << "s\n";
}

// ============= New: Traversal Performance Comparison Tests =============

void test_traversal_performance() {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    // Insert dense data
    for (uint64_t i = 0; i < N; ++i) {
        skiplist[i] = static_cast<int>(i);
    }
    std::cout << "\n========== Traversal Performance Tests ==========\n";
    std::cout << "Data: " << N << " dense elements\n";

    // Test 1: forEach
    auto start = std::chrono::high_resolution_clock::now();
    long long sum1 = 0;
    skiplist.forEach([&sum1](int value, uint64_t) {
        sum1 += value;
        });
    auto end = std::chrono::high_resolution_clock::now();
    double time_forEach = (end - start).count() / 1e9;
    std::cout << "[bsl] forEach: " << time_forEach << "s, sum=" << sum1 << std::endl;

    // Test 2: Iterator (begin/end)
    start = std::chrono::high_resolution_clock::now();
    long long sum2 = 0;
    for (auto it = skiplist.begin(); it != skiplist.end(); ++it) {
        sum2 += *it;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_iterator = (end - start).count() / 1e9;
    std::cout << "[bsl] Iterator: " << time_iterator << "s, sum=" << sum2 << std::endl;

    // Test 3: Reverse iterator (rbegin/rend)
    start = std::chrono::high_resolution_clock::now();
    long long sum3 = 0;
    for (auto it = skiplist.rbegin(); it != skiplist.rend(); --it) {
        sum3 += *it;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_reverse = (end - start).count() / 1e9;
    std::cout << "[bsl] Reverse Iterator: " << time_reverse << "s, sum=" << sum3 << std::endl;

    // Test 4: Random access (operator[])
    start = std::chrono::high_resolution_clock::now();
    long long sum4 = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum4 += skiplist[i];
    }
    end = std::chrono::high_resolution_clock::now();
    double time_random = (end - start).count() / 1e9;
    std::cout << "[bsl] Random Access: " << time_random << "s, sum=" << sum4 << std::endl;

    // Summary
    std::cout << "\n--- Performance Summary (forEach as baseline) ---\n";
    std::cout << "forEach:       " << time_forEach << "s (1.00x)\n";
    std::cout << "Iterator:      " << time_iterator << "s (" << (time_iterator / time_forEach) << "x)\n";
    std::cout << "Reverse Iter:  " << time_reverse << "s (" << (time_reverse / time_forEach) << "x)\n";
    std::cout << "Random Access: " << time_random << "s (" << (time_random / time_forEach) << "x)\n";

    // Verify all sums match
    assert(sum1 == sum2 && sum2 == sum3 && sum3 == sum4);
}

void test_sparse_traversal_performance() {
    const uint64_t N = testCount;
    BitmappedBlockSkipList<uint64_t, int> skiplist(-1);

    // Insert sparse data (10% density)
    for (uint64_t i = 0; i < N; i += 10) {
        skiplist[i] = static_cast<int>(i);
    }
    uint64_t element_count = N / 10;
    std::cout << "\n========== Sparse Traversal Performance Tests ==========\n";
    std::cout << "Data: " << element_count << " sparse elements (10% density)\n";

    // Test 1: forEach
    auto start = std::chrono::high_resolution_clock::now();
    long long sum1 = 0;
    skiplist.forEach([&sum1](int value, uint64_t) {
        sum1 += value;
        });
    auto end = std::chrono::high_resolution_clock::now();
    double time_forEach = (end - start).count() / 1e9;
    std::cout << "[bsl] forEach: " << time_forEach << "s, sum=" << sum1 << std::endl;

    // Test 2: Iterator
    start = std::chrono::high_resolution_clock::now();
    long long sum2 = 0;
    for (auto it = skiplist.begin(); it != skiplist.end(); ++it) {
        sum2 += *it;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_iterator = (end - start).count() / 1e9;
    std::cout << "[bsl] Iterator: " << time_iterator << "s, sum=" << sum2 << std::endl;

    // Test 3: Random access (only existing keys)
    start = std::chrono::high_resolution_clock::now();
    long long sum3 = 0;
    for (uint64_t i = 0; i < N; i += 10) {
        sum3 += skiplist[i];
    }
    end = std::chrono::high_resolution_clock::now();
    double time_random = (end - start).count() / 1e9;
    std::cout << "[bsl] Random Access (existing): " << time_random << "s, sum=" << sum3 << std::endl;

    std::cout << "\n--- Sparse Performance Summary ---\n";
    std::cout << "forEach:  " << time_forEach << "s\n";
    std::cout << "Iterator: " << time_iterator << "s (" << (time_iterator / time_forEach) << "x)\n";
    std::cout << "Random:   " << time_random << "s (" << (time_random / time_forEach) << "x)\n";

    assert(sum1 == sum2 && sum2 == sum3);
}

void test_stdmap_traversal_performance() {
    const uint64_t N = testCount;
    std::map<uint64_t, int> m;

    // Insert dense data
    for (uint64_t i = 0; i < N; ++i) {
        m[i] = static_cast<int>(i);
    }

    std::cout << "\n========== std::map Traversal Performance ==========\n";
    std::cout << "Data: " << N << " dense elements\n";

    // Test 1: Iterator
    auto start = std::chrono::high_resolution_clock::now();
    long long sum1 = 0;
    for (auto it = m.begin(); it != m.end(); ++it) {
        sum1 += it->second;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double time_iterator = (end - start).count() / 1e9;
    std::cout << "[std::map] Iterator: " << time_iterator << "s, sum=" << sum1 << std::endl;

    // Test 2: Range-based for
    start = std::chrono::high_resolution_clock::now();
    long long sum2 = 0;
    for (const auto& pair : m) {
        sum2 += pair.second;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_range = (end - start).count() / 1e9;
    std::cout << "[std::map] Range-for: " << time_range << "s, sum=" << sum2 << std::endl;

    // Test 3: Random access
    start = std::chrono::high_resolution_clock::now();
    long long sum3 = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum3 += m[i];
    }
    end = std::chrono::high_resolution_clock::now();
    double time_random = (end - start).count() / 1e9;
    std::cout << "[std::map] Random Access: " << time_random << "s, sum=" << sum3 << std::endl;

    assert(sum1 == sum2 && sum2 == sum3);
}

void test_vector_traversal_performance() {
    const uint64_t N = testCount;
    std::vector<int> v;

    // Insert dense data
    v.reserve(N); // Pre-allocate to avoid reallocation during insertion
    for (uint64_t i = 0; i < N; ++i) {
        v.push_back(static_cast<int>(i));
    }

    std::cout << "\n========== std::vector Traversal Performance ==========\n";
    std::cout << "Data: " << N << " dense elements\n";

    // Test 1: Iterator
    auto start = std::chrono::high_resolution_clock::now();
    long long sum1 = 0;
    for (auto it = v.begin(); it != v.end(); ++it) {
        sum1 += *it;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double time_iterator = (end - start).count() / 1e9;
    std::cout << "[std::vector] Iterator: " << time_iterator << "s, sum=" << sum1 << std::endl;

    // Test 2: Range-based for
    start = std::chrono::high_resolution_clock::now();
    long long sum2 = 0;
    for (const auto& element : v) {
        sum2 += element;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_range = (end - start).count() / 1e9;
    std::cout << "[std::vector] Range-for: " << time_range << "s, sum=" << sum2 << std::endl;

    // Test 3: Index-based access
    start = std::chrono::high_resolution_clock::now();
    long long sum3 = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum3 += v[i];
    }
    end = std::chrono::high_resolution_clock::now();
    double time_indexed = (end - start).count() / 1e9;
    std::cout << "[std::vector] Indexed Access: " << time_indexed << "s, sum=" << sum3 << std::endl;

    // Test 4: Reverse iterator
    start = std::chrono::high_resolution_clock::now();
    long long sum4 = 0;
    for (auto rit = v.rbegin(); rit != v.rend(); ++rit) {
        sum4 += *rit;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_reverse = (end - start).count() / 1e9;
    std::cout << "[std::vector] Reverse Iterator: " << time_reverse << "s, sum=" << sum4 << std::endl;

    // Test 5: At() method (bounds checked)
    start = std::chrono::high_resolution_clock::now();
    long long sum5 = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum5 += v.at(i);
    }
    end = std::chrono::high_resolution_clock::now();
    double time_at = (end - start).count() / 1e9;
    std::cout << "[std::vector] At() Access: " << time_at << "s, sum=" << sum5 << std::endl;

    // Verify all sums are equal
    assert(sum1 == sum2 && sum2 == sum3 && sum3 == sum4 && sum4 == sum5);
}

void test_hashmap_traversal_performance() {
    const uint64_t N = testCount;
    std::unordered_map<uint64_t, int> hm;

    // Insert dense data
    // Note: unordered_map does not guarantee order, but we insert 0..N
    for (uint64_t i = 0; i < N; ++i) {
        hm[i] = static_cast<int>(i);
    }

    std::cout << "\n========== std::unordered_map Traversal Performance ==========\n";
    std::cout << "Data: " << N << " dense elements\n";

    // Test 1: Iterator
    auto start = std::chrono::high_resolution_clock::now();
    long long sum1 = 0;
    for (auto it = hm.begin(); it != hm.end(); ++it) {
        sum1 += it->second;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double time_iterator = (end - start).count() / 1e9;
    std::cout << "[unordered_map] Iterator: " << time_iterator << "s, sum=" << sum1 << std::endl;

    // Test 2: Range-based for
    start = std::chrono::high_resolution_clock::now();
    long long sum2 = 0;
    for (const auto& pair : hm) {
        sum2 += pair.second;
    }
    end = std::chrono::high_resolution_clock::now();
    double time_range = (end - start).count() / 1e9;
    std::cout << "[unordered_map] Range-for: " << time_range << "s, sum=" << sum2 << std::endl;

    // Test 3: Random access (Key lookup)
    // Note: This is O(1) average for hash map, unlike O(log N) for std::map
    start = std::chrono::high_resolution_clock::now();
    long long sum3 = 0;
    for (uint64_t i = 0; i < N; ++i) {
        sum3 += hm[i];
    }
    end = std::chrono::high_resolution_clock::now();
    double time_random = (end - start).count() / 1e9;
    std::cout << "[unordered_map] Random Access: " << time_random << "s, sum=" << sum3 << std::endl;

    // Verify correctness
    assert(sum1 == sum2 && sum2 == sum3);
}

// ============= Memory Footprint Tests =============

    // exact requested-byte accounting for the allocation seam: every operator new/delete
    // is recorded while a measurement window is open, so the
// reported numbers exclude allocator bucket rounding and per-allocation overhead
// entirely (and transfer to any future custom allocator)
namespace memcount {
    bool enabled = false;
    size_t liveBytes = 0;
    uint64_t allocCount = 0;

    // non-allocating pointer registry: open addressing with tombstones, so deallocation
    // exact sizes are known even for unsized delete calls (trivial-type arrays etc.)
    constexpr size_t kTableBits = 21;
    constexpr size_t kTableSlots = size_t(1) << kTableBits;
    struct Entry { void* ptr; size_t size; };
    Entry table[kTableSlots];   // zero-initialized: ptr == nullptr means empty slot

    size_t slotOf(void* p) {
        return (reinterpret_cast<size_t>(p) >> 4) * 0x9E3779B97F4A7C15ULL >> (64 - kTableBits);
    }

    void record(void* p, size_t n) {
        if (!enabled || p == nullptr) return;
        size_t i = slotOf(p);
        size_t tomb = SIZE_MAX;
        for (size_t probes = 0; probes < kTableSlots; ++probes) {
            if (table[i].ptr == nullptr) {
                if (tomb != SIZE_MAX) i = tomb;
                table[i].ptr = p;
                table[i].size = n;
                liveBytes += n;
                ++allocCount;
                return;
            }
            if (table[i].size == 0 && tomb == SIZE_MAX) tomb = i;
            i = (i + 1) & (kTableSlots - 1);
        }
        // registry saturated: the record is dropped, metrics degrade but nothing crashes
    }

    void release(void* p) {
        if (p == nullptr) return;
        size_t i = slotOf(p);
        for (size_t probes = 0; probes < kTableSlots && table[i].ptr != nullptr; ++probes) {
            if (table[i].ptr == p && table[i].size != 0) {
                if (enabled) liveBytes -= table[i].size;
                table[i].size = 0;   // tombstone, the probe chain stays intact
                return;
            }
            i = (i + 1) & (kTableSlots - 1);
        }
    }

    struct Scope {
        Scope() { liveBytes = 0; allocCount = 0; enabled = true; }
        ~Scope() { enabled = false; }
    };
}

void* operator new(const size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    memcount::record(p, size);
    return p;
}

void* operator new[](const size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    memcount::release(p);
    std::free(p);
}

void operator delete[](void* p) noexcept {
    ::operator delete(p);
}

void operator delete(void* p, const size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, const size_t) noexcept { ::operator delete(p); }

// measures the exact requested bytes of a live container through the allocation seam
template<typename T, typename Make, typename Fill>
void measureMemory(const char* name, uint64_t elementCount, Make&& make, Fill&& fill) {
    size_t live = 0;
    uint64_t allocs = 0;
    {
        memcount::Scope scope;
        std::unique_ptr<T> c = make();
        fill(*c);
        live = memcount::liveBytes;
        allocs = memcount::allocCount;
    }

    const double mb = static_cast<double>(live) / (1024.0 * 1024.0);
    const double bpe = elementCount ? static_cast<double>(live) / static_cast<double>(elementCount) : 0.0;
    std::cout << "  [" << name << "] " << mb << " MB, " << bpe << " B/elem, "
        << allocs << " allocations" << std::endl;
}

// measures a container that reports its own exact usage (e.g. bbsl::memoryUsage traversal)
template<typename T, typename Make, typename Fill, typename Stats>
void measureMemoryWith(const char* name, uint64_t elementCount, Make&& make, Fill&& fill, Stats&& stats) {
    std::unique_ptr<T> c = make();
    fill(*c);
    const auto usage = stats(*c);

    const double mb = static_cast<double>(usage.bytes) / (1024.0 * 1024.0);
    const double bpe = elementCount ? static_cast<double>(usage.bytes) / static_cast<double>(elementCount) : 0.0;
    std::cout << "  [" << name << "] " << mb << " MB, " << bpe << " B/elem, "
        << usage.allocations << " allocations" << std::endl;
}

void test_memory_footprint() {
    const uint64_t N = testCount;

    std::cout << "\n========== Memory Footprint Tests ==========\n";
    std::cout << "(exact requested bytes: allocation seam for std containers, traversal accounting\n";
    std::cout << " for bbsl; allocator overhead excluded)\n";

    // fixed per-instance cost, no heap internals exist while empty
    std::cout << "\n[Empty instance sizeof] (fixed cost per instance)\n";
    std::cout << "  std::vector<int>       : " << sizeof(std::vector<int>) << " bytes\n";
    std::cout << "  std::map<u64,i>        : " << sizeof(std::map<uint64_t, int>) << " bytes\n";
    std::cout << "  std::unordered_map     : " << sizeof(std::unordered_map<uint64_t, int>) << " bytes\n";
    std::cout << "  bbsl skip list         : " << sizeof(BitmappedBlockSkipList<uint64_t, int>) << " bytes\n";

    // ---- dense ----
    std::cout << "\n[Dense: " << N << " elements in [0," << N << ")]\n";
    measureMemory<std::vector<int>>("vector", N,
        [] { return std::make_unique<std::vector<int>>(); },
        [](std::vector<int>& v) {
            v.reserve(testCount);
            for (uint64_t i = 0; i < testCount; ++i) v.push_back(static_cast<int>(i));
        });
    measureMemory<std::map<uint64_t, int>>("std::map", N,
        [] { return std::make_unique<std::map<uint64_t, int>>(); },
        [](std::map<uint64_t, int>& m) {
            for (uint64_t i = 0; i < testCount; ++i) m[i] = static_cast<int>(i);
        });
    measureMemory<std::unordered_map<uint64_t, int>>("unordered_map", N,
        [] { return std::make_unique<std::unordered_map<uint64_t, int>>(); },
        [](std::unordered_map<uint64_t, int>& m) {
            for (uint64_t i = 0; i < testCount; ++i) m[i] = static_cast<int>(i);
        });
    measureMemoryWith<BitmappedBlockSkipList<uint64_t, int>>("bbsl", N,
        [] { return std::make_unique<BitmappedBlockSkipList<uint64_t, int>>(-1); },
        [](BitmappedBlockSkipList<uint64_t, int>& c) {
            for (uint64_t i = 0; i < testCount; ++i) c[i] = static_cast<int>(i);
        },
        [](BitmappedBlockSkipList<uint64_t, int>& c) { return c.memoryUsage(); });

    // ---- sparse family: elementCount keys, stride `step`, so blocks get thinner as step grows ----
    auto sparseCase = [&](const char* title, uint64_t elementCount, uint64_t step) {
        std::cout << "\n[" << title << ": " << elementCount << " elements, key stride " << step << "]\n";
        measureMemory<std::map<uint64_t, int>>("std::map", elementCount,
            [] { return std::make_unique<std::map<uint64_t, int>>(); },
            [&](std::map<uint64_t, int>& m) {
                for (uint64_t i = 0; i < elementCount; ++i) m[i * step] = static_cast<int>(i);
            });
        measureMemory<std::unordered_map<uint64_t, int>>("unordered_map", elementCount,
            [] { return std::make_unique<std::unordered_map<uint64_t, int>>(); },
            [&](std::unordered_map<uint64_t, int>& m) {
                for (uint64_t i = 0; i < elementCount; ++i) m[i * step] = static_cast<int>(i);
            });
        measureMemoryWith<BitmappedBlockSkipList<uint64_t, int>>("bbsl", elementCount,
            [] { return std::make_unique<BitmappedBlockSkipList<uint64_t, int>>(-1); },
            [&](BitmappedBlockSkipList<uint64_t, int>& c) {
                for (uint64_t i = 0; i < elementCount; ++i) c[i * step] = static_cast<int>(i);
            },
            [](BitmappedBlockSkipList<uint64_t, int>& c) { return c.memoryUsage(); });
    };

    sparseCase("Sparse 10%", N / 10, 10);
    sparseCase("Sparse 1%", N / 10, 1'000);
    sparseCase("Scattered (worst case, ~1 element per block)", N / 10, 99'991);
}

int main() {
    std::cout << "Starting data structure `BBSL` benchmark test" << std::endl;
#ifndef NDEBUG
    std::cout << "Running in Debug mode" << std::endl;
#else
    std::cout << "Running in Release mode" << std::endl;
#endif

    // Basic functionality tests
    test1();
    test2();
    test3();
    test4();

    // Generate random seeds using system time and other sources
    auto now = std::chrono::high_resolution_clock::now();
    uint64_t timeSeed = now.time_since_epoch().count();

    std::random_device rd;
    uint64_t seedA = timeSeed ^ (static_cast<uint64_t>(rd()) << 32 | rd());
    uint64_t seedB = seedA ^ 0x5DEECE66DLL;

    std::cout << "\n========== Original Performance Tests (Random Access) ==========\n";
    test_performance_stdmap(seedA);
    test_performance_bsl(seedA);
    test_performance_random_stdmap(seedA, seedB);
    test_performance_random_bsl(seedA, seedB);

    std::cout << "\n========== New: Sequential Access Performance Tests ==========\n";
    test_performance_sequential_stdmap(seedA);
    test_performance_sequential_bsl(seedA);

    std::cout << "\n========== New: Zipf Distribution (Realistic Scenario) ==========\n";
    test_performance_zipf_stdmap(seedA);
    test_performance_zipf_bsl(seedA);

    std::cout << "\n========== New: Range Query Performance Tests ==========\n";
    test_performance_range_stdmap();
    test_performance_range_bsl();
    test_performance_range_scan_bsl();

    std::cout << "\n========== New: Batch Operation Performance Tests ==========\n";
    test_performance_batch_stdmap();
    test_performance_batch_bsl();

    std::cout << "\n========== New: traversal Performance Tests ==========\n";
    test_traversal_performance();
    test_sparse_traversal_performance();
    test_stdmap_traversal_performance();
    test_hashmap_traversal_performance();
    test_vector_traversal_performance();

    test_memory_footprint();
    
    return 0;
}
