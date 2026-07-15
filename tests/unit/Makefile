# Host unit-test lane (Lane A) for the TeraTerm agent fork.
#
#   make          build and run every host test under ASan/UBSan
#   make check    same as `make all`
#   make list     show the test binaries
#   make clean    remove build/
#
# Runs with the system compiler on Linux/macOS. Pure/protocol translation units
# only; Win32-bound paths are shimmed by winhost/ and belong on the native-Windows
# lane. This lane is the fast pre-merge signal wired into .github/workflows/putup.yml.

CC  ?= cc
CXX ?= c++

R      := ../..
TT     := $(R)/teraterm/teraterm
COMMON := $(R)/teraterm/common
CJSON  := $(R)/libs/cJSON
CATCH  := $(R)/libs/catch2
B      := build

SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS   := -g -O1 -Wall $(SAN)
CXXFLAGS := -std=c++17 -g -O1 -Wall $(SAN)

# Header prerequisites: the single-command compiles below can't be tracked by
# the compiler, so list the headers explicitly. Without this, editing a shim or
# a characterized header leaves a stale binary and a false local pass/fail.
WINHOST_H  := winhost/windows.h winhost/crtdbg.h
CODECONV_H := $(COMMON)/codeconv.h $(COMMON)/ttcstd.h $(COMMON)/codemap.h $(CATCH)/catch_amalgamated.hpp $(WINHOST_H)
AGENT_H    := $(wildcard $(TT)/agent_*.h)

# Legacy CHECK()-harness tests (C, self-contained). Kept running verbatim.
AGENT_TESTS := agent_jsonrpc agent_mcp agent_shmem
# Catch2 tests (C++), one binary each.
CATCH_TESTS := codeconv

ALL_TESTS := $(AGENT_TESTS) $(CATCH_TESTS)

.PHONY: all check list clean
all: check
check: $(addprefix run-,$(ALL_TESTS))
	@echo "== all host unit tests passed =="

list:
	@echo $(ALL_TESTS)

run-%: $(B)/%
	@echo "== $* =="
	@$<

$(B):
	@mkdir -p $(B)

# Catch2 framework: compiled once, no sanitizer (it is the harness, not the code
# under test); the ASan/UBSan runtime is still linked in via the test link line.
$(B)/catch_amalgamated.o: $(CATCH)/catch_amalgamated.cpp | $(B)
	$(CXX) -std=c++17 -O1 -I$(CATCH) -c $< -o $@

$(B)/winhost.o: winhost/winhost.c $(WINHOST_H) | $(B)
	$(CC) $(CFLAGS) -Iwinhost -c $< -o $@

# --- legacy agent tests (C) ---
$(B)/agent_jsonrpc: $(TT)/tests/test_agent_jsonrpc.c $(TT)/agent_jsonrpc.c $(CJSON)/cJSON.c $(AGENT_H) | $(B)
	$(CC) $(CFLAGS) -I$(TT) -I$(CJSON) $(filter %.c,$^) -o $@

$(B)/agent_mcp: $(TT)/tests/test_agent_mcp.c $(TT)/agent_mcp.c $(TT)/agent_jsonrpc.c $(CJSON)/cJSON.c $(AGENT_H) | $(B)
	$(CC) $(CFLAGS) -I$(TT) -I$(CJSON) $(filter %.c,$^) -o $@

$(B)/agent_shmem: $(TT)/tests/test_agent_shmem.c $(TT)/agent_shmem.c $(AGENT_H) | $(B)
	$(CC) $(CFLAGS) -I$(TT) $(filter %.c,$^) -o $@

# --- Catch2 tests (C++ + winhost shim) ---
$(B)/codeconv: test_codeconv.cpp $(COMMON)/codeconv.cpp $(B)/winhost.o $(B)/catch_amalgamated.o $(CODECONV_H) | $(B)
	$(CXX) $(CXXFLAGS) -Iwinhost -I$(COMMON) -I$(CATCH) $(filter %.cpp %.o,$^) -o $@

clean:
	@rm -rf $(B)
