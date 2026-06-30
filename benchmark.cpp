#include "../src/order_book.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <chrono>

using namespace lob;
using Clock = std::chrono::high_resolution_clock;

// Small, fast, reproducible PRNG so runs are comparable across machines.
struct Xorshift {
    std::uint64_t s;
    explicit Xorshift(std::uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    std::uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    }
    std::uint32_t range(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

int main() {
    constexpr Price    MID         = 50'000;
    constexpr Price    MAX_PRICE   = 100'000;
    constexpr Price    BAND        = 50;        // orders land within +/- BAND of mid
    constexpr std::size_t SEED_ORDERS = 100'000; // resting depth before timing
    constexpr std::size_t N_OPS    = 5'000'000;  // timed operations

    OrderBook book(MAX_PRICE, N_OPS + SEED_ORDERS + 16);
    Xorshift rng(12345);

    OrderId next_id = 1;
    std::vector<OrderId> live;          // ids we can cancel to keep depth stable
    live.reserve(SEED_ORDERS * 2);

    auto random_price = [&]() -> Price {
        return MID - BAND + rng.range(2 * BAND + 1);
    };

    // Seed both sides with resting liquidity so the timed phase actually matches.
    for (std::size_t i = 0; i < SEED_ORDERS; ++i) {
        const Side side = (i & 1) ? Side::Buy : Side::Sell;
        const Price px  = side == Side::Buy ? MID - 1 - rng.range(BAND)
                                            : MID + 1 + rng.range(BAND);
        const OrderId id = next_id++;
        if (book.submit(id, side, OrderType::Limit, px, 1 + rng.range(100)) > 0)
            live.push_back(id);
    }
    std::printf("seeded %zu resting orders\n", live.size());

    // ---- throughput: a realistic mix of submits and cancels ----------------
    std::vector<std::uint32_t> lat_ns;
    lat_ns.reserve(N_OPS);

    // Measure the clock's own overhead so we can subtract it from each sample.
    std::uint64_t clock_ovh = ~0ull;
    for (int i = 0; i < 1000; ++i) {
        auto a = Clock::now(); auto b = Clock::now();
        auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        if ((std::uint64_t)d < clock_ovh) clock_ovh = d;
    }

    const auto wall_start = Clock::now();
    std::uint64_t cancels = 0, submits = 0;

    for (std::size_t i = 0; i < N_OPS; ++i) {
        // ~30% cancels when we have depth, else submit. Keeps the book from
        // growing without bound and exercises the O(1) cancel path.
        const bool do_cancel = !live.empty() && (rng.range(100) < 30);

        const auto t0 = Clock::now();
        if (do_cancel) {
            const std::size_t k = rng.range((std::uint32_t)live.size());
            const OrderId id = live[k];
            live[k] = live.back(); live.pop_back();
            book.cancel(id);
            ++cancels;
        } else {
            const Side side = (rng.range(2) == 0) ? Side::Buy : Side::Sell;
            const OrderId id = next_id++;
            const Quantity rested =
                book.submit(id, side, OrderType::Limit, random_price(), 1 + rng.range(100));
            if (rested > 0) live.push_back(id);
            ++submits;
        }
        const auto t1 = Clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        ns = ns > (long long)clock_ovh ? ns - clock_ovh : 0;
        lat_ns.push_back((std::uint32_t)ns);
    }

    const auto wall_end = Clock::now();
    const double secs =
        std::chrono::duration<double>(wall_end - wall_start).count();

    // ---- clean throughput: identical workload, zero per-op instrumentation -
    // This is the honest max-throughput figure. The loop above pays for two
    // clock reads and a push_back on every op; this one pays for neither.
    OrderBook book2(MAX_PRICE, N_OPS + SEED_ORDERS + 16);
    Xorshift rng2(12345);
    OrderId nid = 1;
    std::vector<OrderId> live2; live2.reserve(SEED_ORDERS * 2);
    for (std::size_t i = 0; i < SEED_ORDERS; ++i) {
        const Side side = (i & 1) ? Side::Buy : Side::Sell;
        const Price px  = side == Side::Buy ? MID - 1 - rng2.range(BAND)
                                            : MID + 1 + rng2.range(BAND);
        const OrderId id = nid++;
        if (book2.submit(id, side, OrderType::Limit, px, 1 + rng2.range(100)) > 0)
            live2.push_back(id);
    }
    const auto c0 = Clock::now();
    for (std::size_t i = 0; i < N_OPS; ++i) {
        const bool do_cancel = !live2.empty() && (rng2.range(100) < 30);
        if (do_cancel) {
            const std::size_t k = rng2.range((std::uint32_t)live2.size());
            const OrderId id = live2[k];
            live2[k] = live2.back(); live2.pop_back();
            book2.cancel(id);
        } else {
            const Side side = (rng2.range(2) == 0) ? Side::Buy : Side::Sell;
            const OrderId id = nid++;
            const Price px = MID - BAND + rng2.range(2 * BAND + 1);
            if (book2.submit(id, side, OrderType::Limit, px, 1 + rng2.range(100)) > 0)
                live2.push_back(id);
        }
    }
    const auto c1 = Clock::now();
    const double clean_secs = std::chrono::duration<double>(c1 - c0).count();

    // ---- report ------------------------------------------------------------
    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) {
        return lat_ns[(std::size_t)(p * (lat_ns.size() - 1))];
    };

    std::printf("\n--- workload ---\n");
    std::printf("operations      : %zu  (%llu submits, %llu cancels)\n",
                N_OPS, (unsigned long long)submits, (unsigned long long)cancels);
    std::printf("clock overhead  : %llu ns (subtracted)\n",
                (unsigned long long)clock_ovh);

    (void)secs;
    std::printf("\n--- throughput (clean, uninstrumented) ---\n");
    std::printf("elapsed         : %.3f s\n", clean_secs);
    std::printf("ops/sec         : %.2f M\n", N_OPS / clean_secs / 1e6);
    std::printf("ns/op (mean)    : %.1f\n", clean_secs * 1e9 / N_OPS);

    std::printf("\n--- per-op latency (ns, instrumented) ---\n");
    std::printf("median (p50)    : %u\n", pct(0.50));
    std::printf("p90             : %u\n", pct(0.90));
    std::printf("p99             : %u\n", pct(0.99));
    std::printf("p99.9           : %u\n", pct(0.999));
    return 0;
}
