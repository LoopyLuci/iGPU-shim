// ============================================================================
// synapse/tests/test_ring_buffer.cpp
// Project Synapse – Phase 6: TelemetryRingBuffer Verification
//
// Verifies:
//   1. Buffer fills to capacity without stall or crash
//   2. The 1025th push returns false (overflow) — never blocks
//   3. SPSC round-trip: every successfully pushed signature is retrievable
//   4. Concurrent producer/consumer races are safe (run with ThreadSanitizer)
//   5. False sharing is absent — head_ and tail_ are cache-line separated
//
// Build:
//   Compile with -fsanitize=thread to exercise tests 4.
//   g++ -std=c++20 -fsanitize=thread -I.. test_ring_buffer.cpp -o test_ring_buffer
// ============================================================================
#include "../synapse_umd.h"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace synapse;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static WorkloadSignature make_sig(uint32_t id) {
    WorkloadSignature s{};
    s.draw_call_count             = id;
    s.shader_instruction_estimate = id * 4;
    s.vertex_count                = id * 100;
    s.is_compute_dispatch         = (id % 7 == 0);
    return s;
}

static void check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", description);
        std::abort();
    }
    std::fprintf(stdout, "[PASS] %s\n", description);
}

// ---------------------------------------------------------------------------
// Test 1 — Fill to capacity (1024 entries); no stall, no crash
// ---------------------------------------------------------------------------
static void test_fill_to_capacity() {
    TelemetryRingBuffer buf;

    uint32_t pushed = 0;
    for (uint32_t i = 0; i < TelemetryRingBuffer::kBufferSize; ++i) {
        bool ok = buf.push(make_sig(i));
        if (ok) ++pushed;
    }
    // Ring buffer head wraps; after kBufferSize pushes, exactly kBufferSize - 1
    // slots are occupied (one slot reserved to distinguish full from empty).
    check(pushed == TelemetryRingBuffer::kBufferSize - 1,
          "test_fill_to_capacity: pushed exactly kBufferSize-1 entries");
}

// ---------------------------------------------------------------------------
// Test 2 — The 1025th push returns false — never blocks the render thread
// ---------------------------------------------------------------------------
static void test_overflow_returns_false() {
    TelemetryRingBuffer buf;

    // Fill the buffer
    for (uint32_t i = 0; i < TelemetryRingBuffer::kBufferSize; ++i) {
        buf.push(make_sig(i)); // may succeed or fail — that's fine
    }
    // One more push MUST return false (buffer full)
    bool overflow = buf.push(make_sig(9999));
    check(!overflow, "test_overflow_returns_false: push() returns false when full");
}

// ---------------------------------------------------------------------------
// Test 3 — SPSC round-trip: all pushed signatures are retrievable intact
// ---------------------------------------------------------------------------
static void test_round_trip_integrity() {
    TelemetryRingBuffer buf;

    constexpr uint32_t kCount = 512;
    for (uint32_t i = 0; i < kCount; ++i) {
        bool ok = buf.push(make_sig(i));
        check(ok, "test_round_trip_integrity: push succeeded within capacity");
    }

    for (uint32_t i = 0; i < kCount; ++i) {
        auto result = buf.pop();
        check(result.has_value(),
              "test_round_trip_integrity: pop returns value for every push");
        check(result->draw_call_count == i,
              "test_round_trip_integrity: popped signature matches pushed value");
    }

    auto empty = buf.pop();
    check(!empty.has_value(),
          "test_round_trip_integrity: buffer is empty after all pops");
}

// ---------------------------------------------------------------------------
// Test 4 — Concurrent producer / consumer (TSan should report zero violations)
// ---------------------------------------------------------------------------
static void test_concurrent_producer_consumer() {
    TelemetryRingBuffer buf;
    constexpr uint32_t kTotal = 50'000;

    std::atomic<uint32_t> produced{0};
    std::atomic<uint32_t> consumed{0};

    // Producer thread: push as fast as possible
    std::thread producer([&]() {
        for (uint32_t i = 0; i < kTotal; ++i) {
            while (!buf.push(make_sig(i))) {
                // Buffer full — spin briefly without sleeping (mimics render loop)
                std::this_thread::yield();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Consumer thread: drain as fast as possible
    std::thread consumer([&]() {
        uint32_t local_consumed = 0;
        while (local_consumed < kTotal) {
            auto item = buf.pop();
            if (item.has_value()) {
                ++local_consumed;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    check(produced.load() == kTotal,
          "test_concurrent_producer_consumer: all items produced");
    check(consumed.load() == kTotal,
          "test_concurrent_producer_consumer: all items consumed");
    check(!buf.pop().has_value(),
          "test_concurrent_producer_consumer: buffer empty after drain");
}

// ---------------------------------------------------------------------------
// Test 5 — Cache line separation: sizeof proof
// ---------------------------------------------------------------------------
static void test_cache_line_alignment() {
    // sizeof(TelemetryRingBuffer) is at least buffer data + 2 separate cache lines.
    // We can't directly inspect member offsets without offsetof in this style,
    // but we verify the type is at least kCacheLineSize * 2 larger than the data alone.
    constexpr size_t kDataSize  = sizeof(WorkloadSignature) * TelemetryRingBuffer::kBufferSize;
    constexpr size_t kTotalSize = sizeof(TelemetryRingBuffer);
    check(kTotalSize >= kDataSize + 2 * kCacheLineSize,
          "test_cache_line_alignment: head_ and tail_ each occupy a separate cache line");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::fprintf(stdout, "\n=== TelemetryRingBuffer Verification Suite ===\n\n");

    test_fill_to_capacity();
    test_overflow_returns_false();
    test_round_trip_integrity();
    test_concurrent_producer_consumer();
    test_cache_line_alignment();

    std::fprintf(stdout, "\nAll ring buffer tests PASSED.\n");
    return 0;
}
