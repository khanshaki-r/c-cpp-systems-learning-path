# Makefile for 08_Networking_Distributed/code. Run inside WSL Ubuntu 24.04 (GCC 13).
#   make          build every C++ program into ./bin
#   make test     run the self-checking programs (parser tests, token bucket, HLC, Raft, ring)
#   make clean    remove ./bin
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -pthread -O2 -g

SRCS := $(wildcard m*.cpp)
BINS := $(patsubst %.cpp,bin/%,$(SRCS))

# Headers every program may include; rebuilding on header change keeps things honest.
HDRS := $(wildcard *.hpp)

.PHONY: all test clean

all: $(BINS)

bin/%: %.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin:
	mkdir -p bin

# Programs that check themselves with assert and exit non-zero on failure. The servers
# (echo, threadpool, epoll, http) need a client and are exercised by hand as each module shows.
TESTS := bin/m04_tcpkv_parser_test bin/m04_framing bin/m07_rpc_demo bin/m09_token_bucket \
         bin/m10_hlc bin/m11_raft_election_sim bin/m12_consistent_hash

test: $(TESTS)
	for t in $(TESTS); do echo "== $$t"; ./$$t > /dev/null || { echo "FAILED: $$t"; exit 1; }; done
	echo "all tests passed"

clean:
	rm -f $(BINS)
