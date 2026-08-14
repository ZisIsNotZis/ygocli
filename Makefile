TARGET := ygocli
SRC := ygocli.cpp net/net.cpp
OCGCORE_SRCS := $(wildcard ocgcore/*.cpp)

CXX := g++
CXXFLAGS := -g -O0 -std=c++17 -Iocgcore -Inet -I/home/z/llama.cpp/vendor
LDFLAGS :=
LDLIBS := -lsqlite3 -lpthread -ldl

LUA_CFLAGS := $(shell pkg-config --cflags lua5.3 2>/dev/null)
LUA_LIBS := $(shell pkg-config --libs lua5.3 2>/dev/null)

ifeq ($(strip $(LUA_CFLAGS)),)
LUA_CFLAGS := -I/usr/include/lua5.3
endif
ifeq ($(strip $(LUA_LIBS)),)
LUA_LIBS := -llua5.3
endif

CXXFLAGS += $(LUA_CFLAGS)
LDLIBS += $(LUA_LIBS)

.PHONY: all clean test-mcp-solo test-mcp-net

all: $(TARGET)

$(TARGET): $(SRC) $(OCGCORE_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(OCGCORE_SRCS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

# MCP solo: no forced (pass-only) prompt may ever reach the agent.
test-mcp-solo: $(TARGET)
	python3 tests/mcp_auto_pass_test.py example.ydk

# MCP network: same invariant over a live server + two clients.
test-mcp-net: $(TARGET)
	python3 tests/mcp_auto_pass_net_test.py example.ydk
