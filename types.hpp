#pragma once

#include <cstdint>

namespace lob {

// Prices are stored as integer ticks, not floating point. Real exchanges
// quote in discrete ticks, and integers give exact comparisons plus let us
// index price levels directly into a flat array (see OrderBook).
using Price    = std::uint32_t;
using Quantity = std::uint64_t;
using OrderId  = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // rest on the book if it doesn't fully cross
    Market,  // match against any price, cancel the remainder
    IOC,     // immediate-or-cancel: match what you can now, cancel the rest
    FOK      // fill-or-kill: fill the whole quantity now or do nothing
};

// A resting order, stored inside the OrderBook's pool. prev/next are pool
// indices (not pointers) forming the intrusive FIFO list at a price level.
// Using indices keeps every order in one contiguous vector, which is far
// friendlier to the cache than chasing heap pointers around memory.
struct Order {
    OrderId  id;
    Price    price;
    Quantity quantity;
    Side     side;
    std::uint32_t prev;   // previous order at this price level
    std::uint32_t next;   // next order at this price level
};

// Emitted whenever an aggressive order matches a resting one.
struct Trade {
    OrderId  taker_id;     // the incoming, aggressive order
    OrderId  maker_id;     // the resting order that got hit
    Price    price;        // trades execute at the resting (maker) price
    Quantity quantity;
};

} // namespace lob
