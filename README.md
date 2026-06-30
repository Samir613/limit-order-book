# Limit Order Book & Matching Engine

A single-threaded limit order book with price-time priority matching, written in
C++17. It supports limit, market, IOC, and FOK orders with O(1) cancellation,
and sustains 18M+ operations/second single-threaded with a median latency of
40ns per operation.

## Results

Measured on a Linux x86-64 machine, `-O3 -march=native`, with the book
pre-seeded to 100k resting orders and a workload of 5M operations
(70% submits, 30% cancels):

| Metric | Value |
| --- | --- |
| Throughput | 18.3M ops/sec |
| Mean latency | 55 ns/op |
| Median (p50) | 40 ns |
| p99 | 340 ns |
| p99.9 | 641 ns |

Reproduce with `make bench && ./benchmark`. Numbers vary by machine; the
benchmark prints its own.

## Design

The book is built around three decisions, each made for latency.

**Price levels are a flat array indexed by integer price.** Finding the level
at a given price is one array access, not a tree traversal. Prices are integer
ticks rather than floats, which gives exact comparison and makes the array
indexing trivial. The tradeoff is memory proportional to the price range, which
is bounded and cheap for any real instrument.

**Each level is an intrusive doubly linked list of orders in FIFO order.** This
gives time priority directly: the order at the head is the oldest, so it fills
first. Appending a new order and removing an arbitrary order are both O(1).

**Orders live in a pre-allocated pool with a free list.** There is no
`malloc`/`free` in the hot path, which removes allocator latency spikes and
keeps orders contiguous in memory for better cache behavior. The list links are
pool indices rather than raw pointers, so the whole book stays in one vector.

Order-id lookup is a direct-indexed vector because this book assigns ids
sequentially. A venue accepting externally supplied sparse ids would replace
that with a hash map or an open-addressed slab; it's the one spot where the
current design trades generality for speed, and it's called out in the source.

## Order types

| Type | Behavior |
| --- | --- |
| Limit | Match what crosses, rest the remainder on the book |
| Market | Match against any price, cancel the remainder |
| IOC | Match what's available now, cancel the rest, never rest |
| FOK | Fill the entire quantity now or do nothing |

## Build & run

```
make test    # correctness tests (time/price priority, IOC, FOK, cancel, market)
make bench   # build the benchmark, then ./benchmark
make demo    # a short narrated session, then ./demo
```

## Layout

```
src/types.hpp        order, trade, side, and type definitions
src/order_book.hpp   the book and matching engine
src/demo.cpp         a small runnable example
test/test_book.cpp   correctness tests
bench/benchmark.cpp  throughput and latency harness
```

## Possible extensions

- A live feed handler that reconstructs the book from a real exchange's L2
  websocket stream (Coinbase and Binance publish full depth for free).
- Self-trade prevention and order modification with priority rules.
- A second matching thread fed by a lock-free SPSC queue.
