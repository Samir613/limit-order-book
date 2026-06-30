CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra
RELEASE  := -O3 -march=native -DNDEBUG

.PHONY: all demo test bench clean

all: test bench demo

# Tests run without -march=native so they build anywhere; correctness, not speed.
test: test/test_book.cpp src/order_book.hpp src/types.hpp
	$(CXX) $(CXXFLAGS) -O2 test/test_book.cpp -o test_book
	./test_book

bench: bench/benchmark.cpp src/order_book.hpp src/types.hpp
	$(CXX) $(CXXFLAGS) $(RELEASE) bench/benchmark.cpp -o benchmark

demo: src/demo.cpp src/order_book.hpp src/types.hpp
	$(CXX) $(CXXFLAGS) -O2 src/demo.cpp -o demo

clean:
	rm -f test_book benchmark demo
