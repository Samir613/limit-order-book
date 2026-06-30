#include "../src/order_book.hpp"
#include <cstdio>
#include <vector>
#include <cassert>

using namespace lob;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ \
    std::printf("  FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while(0)

// price-time priority: the older order at a price fills first
static void test_time_priority() {
    OrderBook book(1000);
    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t){ trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 100, 10); // rests first
    book.submit(2, Side::Sell, OrderType::Limit, 100, 10); // rests second
    book.submit(3, Side::Buy,  OrderType::Limit, 100, 15); // crosses both

    CHECK(trades.size() == 2);
    CHECK(trades[0].maker_id == 1 && trades[0].quantity == 10); // older first
    CHECK(trades[1].maker_id == 2 && trades[1].quantity == 5);  // partial
    Price ask; CHECK(book.best_ask(ask) && ask == 100);         // 5 left at 100
    std::printf("time_priority: %zu trades\n", trades.size());
}

// price priority: better-priced resting orders fill before worse ones
static void test_price_priority() {
    OrderBook book(1000);
    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t){ trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 102, 10);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 10); // best ask
    book.submit(3, Side::Sell, OrderType::Limit, 101, 10);
    book.submit(4, Side::Buy,  OrderType::Limit, 102, 25); // sweeps up the book

    CHECK(trades.size() == 3);
    CHECK(trades[0].price == 100); // cheapest ask first
    CHECK(trades[1].price == 101);
    CHECK(trades[2].price == 102 && trades[2].quantity == 5);
    std::printf("price_priority: filled across %zu levels\n", trades.size());
}

static void test_ioc() {
    OrderBook book(1000);
    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t){ trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    Quantity rested = book.submit(2, Side::Buy, OrderType::IOC, 100, 12);
    CHECK(rested == 0);            // IOC never rests
    CHECK(trades.size() == 1 && trades[0].quantity == 5); // took the 5 available
    Price bid; CHECK(!book.best_bid(bid)); // remainder cancelled, book empty bid
    std::printf("ioc: took %llu, cancelled the rest\n",
                (unsigned long long)trades[0].quantity);
}

static void test_fok() {
    OrderBook book(1000);
    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t){ trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    Quantity r1 = book.submit(2, Side::Buy, OrderType::FOK, 100, 10);
    CHECK(r1 == 0 && trades.empty()); // can't fully fill -> kill, no trades

    book.submit(3, Side::Sell, OrderType::Limit, 100, 10);
    book.submit(4, Side::Buy,  OrderType::FOK, 100, 12); // 5+10 available
    CHECK(trades.size() == 2);        // now fillable -> executes
    std::printf("fok: kill then fill verified\n");
}

static void test_cancel() {
    OrderBook book(1000);
    int fills = 0;
    book.set_trade_callback([&](const Trade&){ ++fills; });

    book.submit(1, Side::Buy, OrderType::Limit, 100, 10);
    book.submit(2, Side::Buy, OrderType::Limit, 100, 10);
    CHECK(book.cancel(1));            // remove the older order
    CHECK(!book.cancel(1));           // already gone
    book.submit(3, Side::Sell, OrderType::Limit, 100, 10);
    CHECK(fills == 1);               // only order 2 was left to hit
    std::printf("cancel: O(1) removal verified\n");
}

static void test_market() {
    OrderBook book(1000);
    std::vector<Trade> trades;
    book.set_trade_callback([&](const Trade& t){ trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
    book.submit(2, Side::Sell, OrderType::Limit, 105, 10);
    book.submit(3, Side::Buy,  OrderType::Market, 0, 15); // ignores price
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100 && trades[1].price == 105);
    std::printf("market: swept best prices ignoring limit\n");
}

int main() {
    std::printf("running tests...\n");
    test_time_priority();
    test_price_priority();
    test_ioc();
    test_fok();
    test_cancel();
    test_market();
    if (failures == 0) std::printf("\nall tests passed\n");
    else               std::printf("\n%d checks FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
