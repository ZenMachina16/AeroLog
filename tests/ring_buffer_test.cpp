// AeroLog — SPSC Ring Buffer Unit Tests
//
// Self-contained: no external test framework needed — only g++ and pthreads.
// Uses a small 32-slot test ring (not the full 56 MB SharedBuffer) so that
// all boundary conditions are fast to trigger.

#include "../src/shm_shared.hpp"   // provides LogEntry (size assertion)

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal SPSC ring — same algorithm as shm_shared.hpp but with N=32
// so wrap-around and backpressure are cheap to exercise.
// ---------------------------------------------------------------------------
constexpr uint32_t TEST_N = 32;   // must be power of 2
constexpr uint32_t MASK   = TEST_N - 1;

struct TestEntry {
    uint64_t seq;
    uint64_t value;
};

struct TestRing {
    std::atomic<uint64_t> head{0};
    char _hp[64 - sizeof(std::atomic<uint64_t>)]{};
    std::atomic<uint64_t> tail{0};
    char _tp[64 - sizeof(std::atomic<uint64_t>)]{};
    std::atomic<bool>     done{false};
    char _dp[64 - sizeof(std::atomic<bool>)]{};
    TestEntry entries[TEST_N]{};
};

static bool push(TestRing& r, const TestEntry& e) {
    uint64_t t    = r.tail.load(std::memory_order_relaxed);
    uint64_t next = (t + 1) & MASK;
    if (next == r.head.load(std::memory_order_acquire))
        return false;
    r.entries[t] = e;
    r.tail.store(next, std::memory_order_release);
    return true;
}

static bool pop(TestRing& r, TestEntry& out) {
    uint64_t h = r.head.load(std::memory_order_relaxed);
    if (h == r.tail.load(std::memory_order_acquire))
        return false;
    out = r.entries[h];
    r.head.store((h + 1) & MASK, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Lightweight test harness
// ---------------------------------------------------------------------------
static int g_run = 0, g_pass = 0;

#define RUN(name) \
    do { \
        g_run++; \
        std::printf("  %-52s ", #name); \
        std::fflush(stdout); \
        if (test_##name()) { g_pass++; std::puts("PASS"); } \
        else                {           std::puts("FAIL"); } \
    } while(0)

#define ASSERT(cond) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "\n    Assertion failed: %s  (%s:%d)\n", \
                     #cond, __FILE__, __LINE__); \
        return false; \
    } } while(0)

// ---------------------------------------------------------------------------
// Individual tests (each returns true on success, false on failure)
// ---------------------------------------------------------------------------

static bool test_empty_on_init() {
    TestRing r;
    TestEntry e{};
    ASSERT(!pop(r, e));
    ASSERT(r.head.load() == 0);
    ASSERT(r.tail.load() == 0);
    return true;
}

static bool test_single_push_pop() {
    TestRing r;
    TestEntry out{};
    ASSERT(push(r, {42, 99}));
    ASSERT(pop(r, out));
    ASSERT(out.seq   == 42);
    ASSERT(out.value == 99);
    ASSERT(!pop(r, out));    // drained
    return true;
}

// Ring holds exactly N-1 entries before reporting full (one slot is sacrificed
// as the sentinel to distinguish full from empty).
static bool test_capacity_is_n_minus_one() {
    TestRing r;
    int pushed = 0;
    while (push(r, {static_cast<uint64_t>(pushed), 0}))
        pushed++;
    ASSERT(pushed == static_cast<int>(TEST_N - 1));
    ASSERT(!push(r, {999, 0}));   // one more must fail
    return true;
}

// Validate that index wrapping via MASK is correct across two full rounds.
static bool test_wraparound() {
    TestRing r;
    for (uint64_t i = 0; i < TEST_N - 1; ++i)
        ASSERT(push(r, {i, i * 10}));

    TestEntry out{};
    for (uint64_t i = 0; i < TEST_N - 1; ++i) {
        ASSERT(pop(r, out));
        ASSERT(out.seq == i);
    }
    ASSERT(!pop(r, out));    // fully drained

    // Second round — logical indices are now >= TEST_N
    for (uint64_t i = 0; i < TEST_N - 1; ++i)
        ASSERT(push(r, {i + 100, i}));
    for (uint64_t i = 0; i < TEST_N - 1; ++i) {
        ASSERT(pop(r, out));
        ASSERT(out.seq == i + 100);
    }
    return true;
}

// Concurrent SPSC: producer writes N_MSGS sequential entries; consumer reads
// them all and verifies no gaps, no reorderings, and no data loss.
static bool test_concurrent_spsc_no_data_loss() {
    TestRing r;
    constexpr int N_MSGS = 10'000;
    std::vector<uint64_t> received;
    received.reserve(N_MSGS);

    std::thread producer([&]() {
        for (int i = 0; i < N_MSGS; ++i) {
            uint64_t t    = r.tail.load(std::memory_order_relaxed);
            uint64_t next = (t + 1) & MASK;
            while (next == r.head.load(std::memory_order_acquire)) { /* spin */ }
            r.entries[t] = {static_cast<uint64_t>(i), static_cast<uint64_t>(i * 2)};
            r.tail.store(next, std::memory_order_release);
        }
        r.done.store(true, std::memory_order_release);
    });

    while (true) {
        TestEntry e{};
        if (pop(r, e)) {
            received.push_back(e.seq);
        } else if (r.done.load(std::memory_order_acquire) &&
                   r.head.load() == r.tail.load()) {
            break;
        }
    }
    producer.join();

    ASSERT(static_cast<int>(received.size()) == N_MSGS);
    for (int i = 0; i < N_MSGS; ++i)
        ASSERT(received[static_cast<size_t>(i)] == static_cast<uint64_t>(i));
    return true;
}

// Consumer must exit after producer sets done=true and ring is empty.
static bool test_producer_done_signals_consumer() {
    TestRing r;
    bool consumer_exited = false;

    push(r, {1, 10});
    push(r, {2, 20});
    r.done.store(true, std::memory_order_release);

    std::thread consumer([&]() {
        TestEntry e{};
        while (true) {
            if (pop(r, e)) {
                (void)e;
            } else if (r.done.load(std::memory_order_acquire) &&
                       r.head.load() == r.tail.load()) {
                consumer_exited = true;
                break;
            }
        }
    });
    consumer.join();
    ASSERT(consumer_exited);
    return true;
}

// LogEntry must be exactly 56 bytes — the static_assert in shm_shared.hpp
// already catches this at compile time; this test makes ctest report it too.
static bool test_log_entry_abi_size() {
    ASSERT(sizeof(LogEntry) == 56);
    // Verify individual field offsets (matching Python '=Q8sdddIB3xQ')
    ASSERT(offsetof(LogEntry, timestamp_ns)     ==  0);
    ASSERT(offsetof(LogEntry, symbol)           ==  8);
    ASSERT(offsetof(LogEntry, bid)              == 16);
    ASSERT(offsetof(LogEntry, ask)              == 24);
    ASSERT(offsetof(LogEntry, last_price)       == 32);
    ASSERT(offsetof(LogEntry, volume)           == 40);
    ASSERT(offsetof(LogEntry, event_type)       == 44);
    ASSERT(offsetof(LogEntry, write_latency_ns) == 48);
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::printf("AeroLog Ring Buffer Test Suite\n");
    std::printf("══════════════════════════════════════════════════════════\n");

    RUN(empty_on_init);
    RUN(single_push_pop);
    RUN(capacity_is_n_minus_one);
    RUN(wraparound);
    RUN(concurrent_spsc_no_data_loss);
    RUN(producer_done_signals_consumer);
    RUN(log_entry_abi_size);

    std::printf("══════════════════════════════════════════════════════════\n");
    std::printf("Results: %d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
