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
CATCH_TESTS := codeconv telnet_neg buffcell vtparams

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

# codeconv includes [win]-tagged cases whose codepage calls are winhost stubs on
# the host; exclude them here (they run for real on the native ttunittest.exe).
run-codeconv: $(B)/codeconv
	@echo "== codeconv (host: excluding [win]) =="
	@$< "~[win]"

run-buffcell: $(B)/buffcell
	@echo "== buffcell (host: excluding [win]) =="
	@$< "~[win]"

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

# telnet_neg is pure C (no shim); compile it as C, link with the C++ test.
$(B)/telnet_neg_impl.o: $(TT)/telnet_neg.c $(TT)/telnet_neg.h $(TT)/telnet_defs.h | $(B)
	$(CC) $(CFLAGS) -I$(TT) -c $< -o $@

$(B)/telnet_neg: test_telnet_neg.cpp $(TT)/telnet_neg.h $(TT)/telnet_defs.h $(B)/telnet_neg_impl.o $(B)/catch_amalgamated.o | $(B)
	$(CXX) $(CXXFLAGS) -I$(TT) -I$(CATCH) $(filter %.cpp %.o,$^) -o $@

# vtparams is C++ (modern conventions) behind a C-compatible header.
$(B)/vtparams_impl.o: $(TT)/vtparams.cpp $(TT)/vtparams.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(TT) -c $< -o $@

$(B)/vtparams: test_vtparams.cpp $(TT)/vtparams.h $(B)/vtparams_impl.o $(B)/catch_amalgamated.o | $(B)
	$(CXX) $(CXXFLAGS) -I$(TT) -I$(CATCH) $(filter %.cpp %.o,$^) -o $@

# buffcell.c is compiled as C++ on this lane: in C mode ttcstd.h typedefs
# char32_t as unsigned long (8 bytes on LP64), which would give the impl and
# the C++ test different buff_char_t layouts. The native ttunittest lane
# compiles it as genuine C, where the sizes agree.
$(B)/buffcell_impl.o: $(TT)/buffcell.c $(TT)/buffcell.h $(CODECONV_H) | $(B)
	$(CXX) $(CXXFLAGS) -x c++ -Iwinhost -I$(TT) -I$(COMMON) -c $< -o $@

$(B)/buffcell: test_buffcell.cpp $(TT)/buffcell.h $(COMMON)/codeconv.cpp $(B)/buffcell_impl.o $(B)/winhost.o $(B)/catch_amalgamated.o $(CODECONV_H) | $(B)
	$(CXX) $(CXXFLAGS) -Iwinhost -I$(TT) -I$(COMMON) -I$(CATCH) $(filter %.cpp %.o,$^) -o $@

clean:
	@rm -rf $(B)
