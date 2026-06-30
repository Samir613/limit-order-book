#pragma once

#include "types.hpp"
#include <vector>
#include <cstdint>
#include <limits>
#include <functional>

namespace lob {

// A price-time priority limit order book with an integrated matching engine.
//
// Design choices and why they matter for latency:
//
//   1. Price levels live in a flat array indexed by integer price. Looking up
//      "the level at price P" is a single array access, O(1), no tree walk.
//      The cost is memory proportional to the price range, which is bounded
//      and cheap for any real instrument.
//
//   2. Each price level is an intrusive doubly linked list of orders, kept in
//      FIFO arrival order. That gives time priority for free and makes both
//      "append a new order" and "remove an arbitrary order" O(1).
//
//   3. Orders live in a pre-allocated pool (a vector) with a free list. No
//      malloc/free in the hot path, so no allocator latency spikes and good
//      cache locality.
//
//   4. order_id -> pool slot is a direct-indexed vector because this book
//      assigns ids sequentially. A production venue handling externally
//      supplied sparse ids would swap this for a hash map or open-addressed
//      slab; that's the one place the design trades generality for speed.
class OrderBook {
public:
    // Called for every fill. Kept as a std::function for clarity; in the
    // latency benchmark we template the match loop to avoid the indirect call.
    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(Price max_price, std::size_t expected_orders = 1u << 20)
        : levels_(max_price + 1),
          order_slot_(expected_orders + 1, kNull),
          best_bid_(0),
          best_ask_(max_price + 1),
          max_price_(max_price) {
        pool_.reserve(expected_orders + 1);
        pool_.emplace_back();          // index 0 is the null sentinel
    }

    void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }

    // Submit an order. Returns the number of units that ended up resting on
    // the book (0 for a fully filled or fully cancelled order).
    Quantity submit(OrderId id, Side side, OrderType type, Price price, Quantity qty) {
        if (type == OrderType::FOK && !fillable(side, price, qty))
            return 0;                  // kill: nothing trades, nothing rests

        Quantity remaining = match(id, side, type, price, qty);

        const bool can_rest =
            (type == OrderType::Limit) && remaining > 0;
        if (can_rest)
            rest(id, side, price, remaining);
        return can_rest ? remaining : 0;
    }

    // Cancel a resting order by id. Returns true if it was on the book.
    bool cancel(OrderId id) {
        if (id >= order_slot_.size()) return false;
        const std::uint32_t slot = order_slot_[id];
        if (slot == kNull) return false;
        unlink(slot);
        order_slot_[id] = kNull;
        free_slot(slot);
        return true;
    }

    // Best prices currently resting. Returns false if that side is empty.
    bool best_bid(Price& out) const {
        if (best_bid_ == 0) return false;
        out = best_bid_; return true;
    }
    bool best_ask(Price& out) const {
        if (best_ask_ > max_price_) return false;
        out = best_ask_; return true;
    }

private:
    static constexpr std::uint32_t kNull = 0;

    struct Level {
        std::uint32_t head = kNull;    // oldest order (front of FIFO)
        std::uint32_t tail = kNull;    // newest order (back of FIFO)
        Quantity      volume = 0;      // total resting quantity at this price
    };

    // ---- pool management ----------------------------------------------------

    std::uint32_t alloc_slot() {
        if (free_head_ != kNull) {     // reuse a recycled slot
            const std::uint32_t s = free_head_;
            free_head_ = pool_[s].next;
            return s;
        }
        pool_.emplace_back();          // grow the pool
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }

    void free_slot(std::uint32_t s) {
        pool_[s].next = free_head_;    // push onto the free list
        free_head_ = s;
    }

    // ---- list surgery at a price level -------------------------------------

    void unlink(std::uint32_t slot) {
        Order& o = pool_[slot];
        Level& lvl = levels_[o.price];
        if (o.prev != kNull) pool_[o.prev].next = o.next;
        else                 lvl.head = o.next;
        if (o.next != kNull) pool_[o.next].prev = o.prev;
        else                 lvl.tail = o.prev;
        lvl.volume -= o.quantity;
        if (lvl.head == kNull)         // level just went empty
            on_level_emptied(o.side, o.price);
    }

    void on_level_emptied(Side side, Price price) {
        // If we just emptied the current best, walk to the next populated
        // level. This scan is the price we pay for O(1) level lookup; it's
        // bounded by the price range and almost always one step.
        if (side == Side::Buy && price == best_bid_) {
            while (best_bid_ > 0 && levels_[best_bid_].head == kNull)
                --best_bid_;
        } else if (side == Side::Sell && price == best_ask_) {
            while (best_ask_ <= max_price_ && levels_[best_ask_].head == kNull)
                ++best_ask_;
        }
    }

    // ---- resting -----------------------------------------------------------

    void rest(OrderId id, Side side, Price price, Quantity qty) {
        const std::uint32_t slot = alloc_slot();
        Order& o = pool_[slot];
        o.id = id; o.price = price; o.quantity = qty; o.side = side;
        o.prev = kNull; o.next = kNull;

        Level& lvl = levels_[price];
        if (lvl.tail == kNull) {       // first order at this price
            lvl.head = lvl.tail = slot;
        } else {                       // append for time priority
            pool_[lvl.tail].next = slot;
            o.prev = lvl.tail;
            lvl.tail = slot;
        }
        lvl.volume += qty;

        if (id < order_slot_.size()) order_slot_[id] = slot;
        else { order_slot_.resize(id + 1, kNull); order_slot_[id] = slot; }

        if (side == Side::Buy)  { if (price > best_bid_) best_bid_ = price; }
        else                    { if (price < best_ask_) best_ask_ = price; }
    }

    // ---- matching ----------------------------------------------------------

    // Returns the unfilled quantity left over after crossing the book.
    Quantity match(OrderId taker, Side side, OrderType type, Price price, Quantity qty) {
        if (side == Side::Buy) {
            const bool market = (type == OrderType::Market);
            while (qty > 0 && best_ask_ <= max_price_ &&
                   (market || best_ask_ <= price)) {
                qty = consume_level(taker, best_ask_, qty);
            }
        } else {
            const bool market = (type == OrderType::Market);
            while (qty > 0 && best_bid_ > 0 &&
                   (market || best_bid_ >= price)) {
                qty = consume_level(taker, best_bid_, qty);
            }
        }
        return qty;
    }

    // Eat into the orders resting at one price level, oldest first, until the
    // taker is satisfied or the level empties. Returns remaining taker qty.
    Quantity consume_level(OrderId taker, Price lvl_price, Quantity qty) {
        Level& lvl = levels_[lvl_price];
        std::uint32_t cur = lvl.head;
        while (cur != kNull && qty > 0) {
            Order& maker = pool_[cur];
            const Quantity fill = qty < maker.quantity ? qty : maker.quantity;

            if (on_trade_)
                on_trade_(Trade{taker, maker.id, lvl_price, fill});

            qty            -= fill;
            maker.quantity -= fill;
            lvl.volume     -= fill;

            if (maker.quantity == 0) {  // maker fully filled, remove it
                const std::uint32_t next = maker.next;
                order_slot_[maker.id] = kNull;
                lvl.head = next;
                if (next != kNull) pool_[next].prev = kNull;
                else               lvl.tail = kNull;
                free_slot(cur);
                cur = next;
            } else {
                cur = kNull;            // partial fill, maker stays on top
            }
        }
        if (lvl.head == kNull) {
            const Side s = (lvl_price == best_ask_) ? Side::Sell : Side::Buy;
            on_level_emptied(s, lvl_price);
        }
        return qty;
    }

    // For FOK: is there enough resting volume within the price limit to fill
    // the whole order right now? Walks levels without mutating anything.
    bool fillable(Side side, Price price, Quantity qty) const {
        Quantity avail = 0;
        if (side == Side::Buy) {
            for (Price p = best_ask_; p <= max_price_ && p <= price; ++p) {
                avail += levels_[p].volume;
                if (avail >= qty) return true;
            }
        } else {
            for (Price p = best_bid_; p > 0 && p >= price; --p) {
                avail += levels_[p].volume;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    std::vector<Order>        pool_;        // all orders, contiguous
    std::vector<Level>        levels_;      // indexed by price
    std::vector<std::uint32_t> order_slot_; // order_id -> pool index
    std::uint32_t             free_head_ = kNull;
    Price                     best_bid_;
    Price                     best_ask_;
    Price                     max_price_;
    TradeCallback             on_trade_;
};

} // namespace lob
