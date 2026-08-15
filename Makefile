# ygomcp — headless MCP duel client (fresh implementation, no ygopro/gframe code).
#
# Layering: proto (wire: socket+framing+STOC/CTOS/MSG codecs) -> game (business
# logic, registers MCP tools) -> mcp (pure JSON-RPC transport). Each layer is
# independently testable; see tests/.
#
# Error-surface ladder (top rung first): -Werror at compile time, TDD per layer,
# random sanity test at integration. No runtime warnings allowed past the build.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -Isrc
LDLIBS    = -lsqlite3

SRC      = src/proto.cpp src/game.cpp src/mcp.cpp src/main.cpp
TEST_SRC = $(wildcard tests/*_test.cpp)

BIN      = ygomcp
TEST_BIN = build/tests

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDLIBS)

$(TEST_BIN): $(filter-out src/main.cpp,$(SRC)) $(TEST_SRC) tests/test_main.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(filter-out src/main.cpp,$(SRC)) $(TEST_SRC) tests/test_main.cpp $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(BIN)
	rm -rf build
