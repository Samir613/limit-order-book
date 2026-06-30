#include "../src/order_book.hpp"
#include <cstdio>

using namespace lob;

// A tiny narrated session so anyone cloning the repo can see the engine work
// without reading the benchmark. Run with: make demo && ./demo
int main() {
    OrderBook book(/*max_price=*/1000);
    book.set_trade_callback([](const Trade& t) {
        std::printf("  TRADE  taker=%llu  maker=%llu  %llu @ %u\n",
                    (unsigned long long)t.taker_id,
                    (unsigned long long)t.maker_id,
                    (unsigned long long)t.quantity, t.price);
    });

    std::printf("resting sell 100 @ 101, sell 50 @ 102\n");
    book.submit(1, Side::Sell, OrderType::Limit, 101, 100);
    book.submit(2, Side::Sell, OrderType::Limit, 102, 50);

    Price ask;
    if (book.best_ask(ask)) std::printf("best ask is now %u\n\n", ask);

    std::printf("buy 120 @ 102 (limit) sweeps the book:\n");
    Quantity rested = book.submit(3, Side::Buy, OrderType::Limit, 102, 120);
    std::printf("  %llu units rested as the new best bid\n\n",
                (unsigned long long)rested);

    Price bid;
    if (book.best_bid(bid)) std::printf("best bid is now %u\n", bid);
    if (book.best_ask(ask)) std::printf("best ask is now %u\n", ask);
    return 0;
}
