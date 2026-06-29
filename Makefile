TARGET := ygocli
SRC := ygocli.cpp
OCGCORE_SRCS := $(wildcard ocgcore/*.cpp)

CXX := g++
CXXFLAGS := -g -O0 -std=c++17 -Iocgcore
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

.PHONY: all clean test-regression test-mass test-fuzz-random

all: $(TARGET)

$(TARGET): $(SRC) $(OCGCORE_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(OCGCORE_SRCS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

test-regression: $(TARGET)
	bash tests/regression_no_retry.sh

test-mass: $(TARGET)
	bash tests/mass_autoplay.sh

test-fuzz-random: $(TARGET)
	bash tests/fuzz_random_choices.sh
