# -----------------------------
# Makefile for chord-dht-filesystem
# -----------------------------
#
#   make            the programs: tracker, node, client, chunk, lookup, sim-hops, sim-balance
#   make check      build and run the seven unit-test binaries
#   make tsan       ThreadSanitizer builds of the node and the client
#   make clean
#
# Layout (decision D-068): sources in src/, unit tests in tests/, object files and
# their dependency files in build/, and the programs at the repository root, where
# every script, the container files and the documentation run them from.

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2 -Isrc

# The largest chunk, in bytes, is one compile-time constant (decision D-063). Only
# the chunk-size sweep changes it, in a separate copy of the tree:
#   make CHUNK_LIMIT=8388608
ifdef CHUNK_LIMIT
CXXFLAGS += -DCHORDFS_MAX_CHUNK=$(CHUNK_LIMIT)
endif

# -MMD -MP: the compiler writes each object's header dependencies to a .d file next
# to it, so editing any header rebuilds exactly the objects that include it. (The
# hand-written dependency lists this replaces had to be kept in step by hand; a
# missing line there links yesterday's code without an error.)
DEPFLAGS = -MMD -MP

# Link-time libraries.
# sha1.h is a header-only wrapper over OpenSSL's SHA1(). The declaration comes from
# <openssl/sha.h> at compile time, but the machine code for SHA1 lives in libcrypto,
# so it must be named at LINK time or the linker reports "undefined reference to
# SHA1" (defect B2; see docs/failures.md). There is no sha1.cpp and there never was:
# listing one is what produced "No rule to make target 'sha1.o'" (defect B1).
CRYPTO_LDLIBS = -lcrypto

BUILD = build
obj = $(addprefix $(BUILD)/,$(addsuffix .o,$(1)))

# The libraries every program is built from, each a socket-free module (except net)
# so the unit tests can drive it in process.
#   ROUTING  the originator's loop and the ring arithmetic
#   RING     a member's routing state and maintenance
#   CHUNKS   the chunk plane: the store, replicated writes, verified reads
NET     = net
ROUTING = lookup chord $(NET)
RING    = stabilize $(ROUTING)
CHUNKS  = chunk store $(RING)

PROGRAMS = tracker client node chunk lookup sim-hops sim-balance
TESTS    = test-chord test-net test-lookup test-stabilize test-chunk test-transfer test-names

all: $(PROGRAMS)

# -----------------------------
# Programs (src/)
# -----------------------------

# The tracker is the namespace (D-058) on the node's framing (D-060). names.o checks
# keys with store.o's valid_key and splits words with lookup.o's helpers.
tracker: $(call obj,tracker names store $(ROUTING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# The client: transfer.o moves whole files with chunk.o's put_chunk and get_chunk;
# names.o only for valid_name, so a bad name is refused before anything is uploaded.
client: $(call obj,client transfer names $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# A ring process: its members' maintenance, the chunk store and re-replication.
node: $(call obj,node repair $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# Tools: one chunk, or one lookup per line of input, through a ring.
chunk: $(call obj,chunk_main $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

lookup: $(call obj,lookup_main $(ROUTING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# Benchmark replays: the same routing (D-027) and placement (D-051) code in one process.
sim-hops: $(call obj,sim_hops $(ROUTING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

sim-balance: $(call obj,sim_balance $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# -----------------------------
# Unit tests (tests/) -- not part of `all`; `make check` builds and runs them
# -----------------------------

test-chord: $(call obj,test_chord chord)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

test-net: $(call obj,test_net $(NET))
	$(CXX) $(CXXFLAGS) -o $@ $^

test-lookup: $(call obj,test_lookup $(ROUTING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

test-stabilize: $(call obj,test_stabilize $(RING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

test-chunk: $(call obj,test_chunk repair $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

test-transfer: $(call obj,test_transfer transfer $(CHUNKS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

test-names: $(call obj,test_names names store $(ROUTING))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# Build and run the in-process tests. Exits non-zero on the first failure, so it can
# be used as a gate.
check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# -----------------------------
# Object files
# -----------------------------

# Every object depends on build/flags, which holds the compiler flags it was built
# with and is rewritten only when they change -- so `make CHUNK_LIMIT=...` after a
# default build recompiles everything instead of linking objects built with the
# other limit.
$(BUILD)/%.o: src/%.cpp $(BUILD)/flags
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: tests/%.cpp $(BUILD)/flags
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CXX) $(CXXFLAGS)' | cmp -s - $@ || echo '$(CXX) $(CXXFLAGS)' > $@

-include $(wildcard $(BUILD)/*.d)

# -----------------------------
# ThreadSanitizer builds (decision D-030), not part of `all`
# -----------------------------
# Every translation unit must be instrumented, so these compile from source rather
# than reusing the normal objects. On this aarch64 kernel ThreadSanitizer only runs
# with address-space randomisation turned off:
#   NODE_CMD="setarch $(uname -m) -R ./node-tsan" scripts/e2e-churn.sh
# The client is multi-threaded too since Phase 5 (D-056): its workers share a
# counter, a stop flag, per-chunk result slots and the part file's descriptor.
#   CLIENT_CMD="setarch $(uname -m) -R ./client-tsan" NODE_CMD=... scripts/e2e-transfer.sh
TSAN_FLAGS = -std=c++17 -Wall -pthread -g -O1 -fsanitize=thread -Isrc
src = $(addprefix src/,$(addsuffix .cpp,$(1)))

node-tsan: $(call src,node repair $(CHUNKS)) $(wildcard src/*.h)
	$(CXX) $(TSAN_FLAGS) -o $@ $(filter %.cpp,$^) $(CRYPTO_LDLIBS)

client-tsan: $(call src,client transfer names $(CHUNKS)) $(wildcard src/*.h)
	$(CXX) $(TSAN_FLAGS) -o $@ $(filter %.cpp,$^) $(CRYPTO_LDLIBS)

tsan: node-tsan client-tsan

clean:
	rm -rf $(BUILD) $(PROGRAMS) $(TESTS) node-tsan client-tsan

.PHONY: all check tsan clean FORCE
