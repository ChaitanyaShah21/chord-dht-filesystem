# -----------------------------
# Makefile for P2P File Sharing
# -----------------------------

# Compiler and flags
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2

# The largest chunk, in bytes, is one compile-time constant (decision D-063). Only
# the chunk-size sweep changes it, in a separate copy of the tree:
#   make CHUNK_LIMIT=8388608
ifdef CHUNK_LIMIT
CXXFLAGS += -DCHORDFS_MAX_CHUNK=$(CHUNK_LIMIT)
endif

# Link-time libraries.
# client.cpp includes sha1.h, which is a header-only wrapper over OpenSSL's SHA1().
# The declaration comes from <openssl/sha.h> at compile time, but the machine code for
# SHA1 lives in libcrypto, so it must be named at LINK time or the linker reports
# "undefined reference to SHA1". tracker.cpp does not hash anything today, so it does
# not need it (defect B2; see docs/failures.md).
CRYPTO_LDLIBS = -lcrypto

# Executables
TRACKER = tracker
CLIENT  = client
NODE    = node
LOOKUP  = lookup
SIM_HOPS = sim-hops
CHUNK    = chunk
SIM_BALANCE = sim-balance

# test-chord, test-net, test-lookup, test-stabilize and test-chunk are TEST
# binaries, not deliverables: they exercise the routing arithmetic, the framing,
# the lookup loop, the ring's maintenance and the chunk plane in process,
# with rings built by hand. They are deliberately not part of `all`, so a normal
# build never depends on them; `make check` builds and runs them.
TEST_CHORD = test-chord
TEST_NET   = test-net
TEST_LOOKUP = test-lookup
TEST_STABILIZE = test-stabilize
TEST_CHUNK     = test-chunk
TEST_TRANSFER  = test-transfer
TEST_NAMES     = test-names

# Source files.
# There is no sha1.cpp and there never was: sha1.h is header-only (every function is
# `inline`, so it is compiled into each translation unit that includes it). Listing a
# non-existent sha1.cpp here is what produced "No rule to make target 'sha1.o'" (B1).
TRACKER_SRC = tracker.cpp
CLIENT_SRC  = client.cpp

# chord.cpp holds the routing primitives as free functions (decision D-018), so
# they can be linked into a test binary with no sockets involved. It includes
# sha1.h, so anything linking it needs -lcrypto.
CHORD_SRC      = chord.cpp
TEST_CHORD_SRC = test_chord.cpp

# The ring member (decision D-018) and its line framing (decision D-021).
NODE_SRC = node.cpp
NET_SRC  = net.cpp
TEST_NET_SRC = test_net.cpp

# The lookup originator (decision D-024). lookup.cpp is the loop, with the
# request/reply exchange passed in, so the tests can drive it with constructed
# rings; lookup_main.cpp is the thin command-line tool around it.
LOOKUP_SRC      = lookup.cpp
LOOKUP_MAIN_SRC = lookup_main.cpp
TEST_LOOKUP_SRC = test_lookup.cpp

# A ring member's routing state and everything that changes it: joins,
# stabilisation, failure handling (Phase 3). Like lookup.cpp it never opens a
# socket -- calls to other nodes go through an injected transport -- so
# test_stabilize.cpp runs a whole ring in one process.
STABILIZE_SRC      = stabilize.cpp
TEST_STABILIZE_SRC = test_stabilize.cpp

# The chunk plane (Phase 4): the store on disk (store.cpp), replicated writes and
# verified reads (chunk.cpp), and re-replication (repair.cpp). Like stabilize.cpp,
# none of them opens a socket -- calls go through an injected transport -- so
# test_chunk.cpp runs several processes' stores and ring members in one process.
# chunk_main.cpp is the command-line client.
STORE_SRC      = store.cpp
CHUNK_SRC      = chunk.cpp
REPAIR_SRC     = repair.cpp
CHUNK_MAIN_SRC = chunk_main.cpp
TEST_CHUNK_SRC = test_chunk.cpp

# Whole files through the ring (Phase 5): chunking, manifests and their tree, the
# worker pool and the part file (transfer.cpp). Storing and fetching one chunk are
# passed in, so test_transfer.cpp runs whole transfers against a ring in memory.
TRANSFER_SRC      = transfer.cpp
TEST_TRANSFER_SRC = test_transfer.cpp

# The tracker's namespace (decision D-058): name -> manifest key and its log, with
# no sockets, so test_names.cpp drives it with request lines and real log files.
NAMES_SRC      = names.cpp
TEST_NAMES_SRC = test_names.cpp

# The load-balance simulation (decision D-051): placement computed with the node's
# own identifier and replica-set code, for rings far larger than fit as processes.
SIM_BALANCE_SRC = sim_balance.cpp

# The hop-count benchmark's in-process replay (decision D-027): the same routing
# code and lookup loop as the real ring, with a function call for a transport.
SIM_HOPS_SRC = sim_hops.cpp

# Object files
TRACKER_OBJ    = $(TRACKER_SRC:.cpp=.o)
CLIENT_OBJ     = $(CLIENT_SRC:.cpp=.o)
CHORD_OBJ      = $(CHORD_SRC:.cpp=.o)
TEST_CHORD_OBJ = $(TEST_CHORD_SRC:.cpp=.o)
NODE_OBJ       = $(NODE_SRC:.cpp=.o)
NET_OBJ        = $(NET_SRC:.cpp=.o)
TEST_NET_OBJ   = $(TEST_NET_SRC:.cpp=.o)
LOOKUP_OBJ      = $(LOOKUP_SRC:.cpp=.o)
LOOKUP_MAIN_OBJ = $(LOOKUP_MAIN_SRC:.cpp=.o)
TEST_LOOKUP_OBJ = $(TEST_LOOKUP_SRC:.cpp=.o)
SIM_HOPS_OBJ    = $(SIM_HOPS_SRC:.cpp=.o)
STABILIZE_OBJ      = $(STABILIZE_SRC:.cpp=.o)
TEST_STABILIZE_OBJ = $(TEST_STABILIZE_SRC:.cpp=.o)
STORE_OBJ          = $(STORE_SRC:.cpp=.o)
CHUNK_OBJ          = $(CHUNK_SRC:.cpp=.o)
REPAIR_OBJ         = $(REPAIR_SRC:.cpp=.o)
CHUNK_MAIN_OBJ     = $(CHUNK_MAIN_SRC:.cpp=.o)
TEST_CHUNK_OBJ     = $(TEST_CHUNK_SRC:.cpp=.o)
SIM_BALANCE_OBJ    = $(SIM_BALANCE_SRC:.cpp=.o)
TRANSFER_OBJ       = $(TRANSFER_SRC:.cpp=.o)
TEST_TRANSFER_OBJ  = $(TEST_TRANSFER_SRC:.cpp=.o)
NAMES_OBJ          = $(NAMES_SRC:.cpp=.o)
TEST_NAMES_OBJ     = $(TEST_NAMES_SRC:.cpp=.o)

# Default target
all: $(TRACKER) $(CLIENT) $(NODE) $(LOOKUP) $(SIM_HOPS) $(CHUNK) $(SIM_BALANCE)

# -----------------------------
# Build rules
# -----------------------------

$(TRACKER): $(TRACKER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(CLIENT): $(CLIENT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# The node answers requests through stabilize.o and chunk.o, and joins and repairs
# fingers with the client's own lookup loop, so it links lookup.o as well.
$(NODE): $(NODE_OBJ) $(REPAIR_OBJ) $(CHUNK_OBJ) $(STORE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# The chunk client: chunk.o's put_chunk/get_chunk, which use the lookup loop and
# read successor lists with stabilize.o's parser.
$(CHUNK): $(CHUNK_MAIN_OBJ) $(CHUNK_OBJ) $(STORE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# lookup links chord.o (identifiers, interval tests), so it needs -lcrypto too.
$(LOOKUP): $(LOOKUP_MAIN_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(SIM_HOPS): $(SIM_HOPS_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_CHORD): $(TEST_CHORD_OBJ) $(CHORD_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_NET): $(TEST_NET_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# test-lookup drives lookup.o with rings built in process. It never opens a
# socket, but lookup.o contains socket_transport, so net.o must be linked.
$(TEST_LOOKUP): $(TEST_LOOKUP_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_STABILIZE): $(TEST_STABILIZE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(SIM_BALANCE): $(SIM_BALANCE_OBJ) $(CHUNK_OBJ) $(STORE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_TRANSFER): $(TEST_TRANSFER_OBJ) $(TRANSFER_OBJ) $(CHUNK_OBJ) $(STORE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# names.o checks keys with store.o's valid_key and splits words with lookup.o's
# split_single_spaces, so it links them (and what they need).
$(TEST_NAMES): $(TEST_NAMES_OBJ) $(NAMES_OBJ) $(STORE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_CHUNK): $(TEST_CHUNK_OBJ) $(REPAIR_OBJ) $(CHUNK_OBJ) $(STORE_OBJ) $(STABILIZE_OBJ) $(LOOKUP_OBJ) $(CHORD_OBJ) $(NET_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# Rebuild any object file if sha1.h changes. Without this, editing the header leaves
# stale .o files behind and the next `make` links yesterday's code.
$(CLIENT_OBJ): sha1.h
$(CHORD_OBJ): chord.h sha1.h
$(TEST_CHORD_OBJ): chord.h
$(NODE_OBJ): repair.h chunk.h store.h stabilize.h lookup.h chord.h net.h
$(NET_OBJ): net.h
$(TEST_NET_OBJ): net.h
# lookup.h includes both chord.h and net.h, so anything including it depends on all three.
$(LOOKUP_OBJ): lookup.h chord.h net.h
$(LOOKUP_MAIN_OBJ): lookup.h chord.h net.h
$(TEST_LOOKUP_OBJ): lookup.h chord.h net.h
$(SIM_HOPS_OBJ): lookup.h chord.h net.h
# stabilize.h includes lookup.h, which includes chord.h and net.h.
$(STABILIZE_OBJ): stabilize.h lookup.h chord.h net.h
$(TEST_STABILIZE_OBJ): stabilize.h lookup.h chord.h net.h
$(STORE_OBJ): store.h chord.h sha1.h
# chunk.h includes stabilize.h, store.h, lookup.h, chord.h and net.h.
$(CHUNK_OBJ): chunk.h store.h stabilize.h lookup.h chord.h net.h
$(REPAIR_OBJ): repair.h chunk.h store.h stabilize.h lookup.h chord.h net.h
$(CHUNK_MAIN_OBJ): chunk.h store.h stabilize.h lookup.h chord.h net.h
$(TEST_CHUNK_OBJ): repair.h chunk.h store.h stabilize.h lookup.h chord.h net.h
$(TRANSFER_OBJ): transfer.h chunk.h store.h stabilize.h lookup.h chord.h net.h
$(TEST_TRANSFER_OBJ): transfer.h chunk.h store.h stabilize.h lookup.h chord.h net.h
$(NAMES_OBJ): names.h lookup.h chord.h net.h store.h sha1.h
$(TEST_NAMES_OBJ): names.h net.h store.h chord.h
$(SIM_BALANCE_OBJ): chunk.h store.h stabilize.h lookup.h chord.h net.h sha1.h

# A ThreadSanitizer build of the node, for looking for data races between the
# maintenance thread and the request threads (decision D-030). Every translation
# unit must be instrumented, so it compiles from source rather than reusing the
# normal objects. Not part of `all`. On this aarch64 kernel ThreadSanitizer only
# runs with address-space randomisation turned off:
#   NODE_CMD="setarch $$(uname -m) -R ./node-tsan" scripts/e2e-churn.sh
NODE_TSAN = node-tsan
NODE_TSAN_SRC = $(NODE_SRC) $(REPAIR_SRC) $(CHUNK_SRC) $(STORE_SRC) $(STABILIZE_SRC) $(LOOKUP_SRC) $(CHORD_SRC) $(NET_SRC)

$(NODE_TSAN): $(NODE_TSAN_SRC) repair.h chunk.h store.h stabilize.h lookup.h chord.h net.h sha1.h
	$(CXX) -std=c++17 -Wall -pthread -g -O1 -fsanitize=thread -o $@ $(NODE_TSAN_SRC) $(CRYPTO_LDLIBS)

tsan: $(NODE_TSAN)

# -----------------------------
# Clean rule
# -----------------------------
clean:
	rm -f $(TRACKER_OBJ) $(CLIENT_OBJ) $(CHORD_OBJ) $(TEST_CHORD_OBJ) $(NODE_OBJ) $(NET_OBJ) $(TEST_NET_OBJ) \
	      $(LOOKUP_OBJ) $(LOOKUP_MAIN_OBJ) $(TEST_LOOKUP_OBJ) $(SIM_HOPS_OBJ) $(STABILIZE_OBJ) $(TEST_STABILIZE_OBJ) \
	      $(STORE_OBJ) $(CHUNK_OBJ) $(REPAIR_OBJ) $(CHUNK_MAIN_OBJ) $(TEST_CHUNK_OBJ) $(SIM_BALANCE_OBJ) \
	      $(TRANSFER_OBJ) $(TEST_TRANSFER_OBJ) $(TEST_TRANSFER) $(NAMES_OBJ) $(TEST_NAMES_OBJ) $(TEST_NAMES) \
	      $(TRACKER) $(CLIENT) $(NODE) $(LOOKUP) $(SIM_HOPS) $(CHUNK) $(SIM_BALANCE) $(TEST_CHORD) $(TEST_NET) $(TEST_LOOKUP) \
	      $(TEST_STABILIZE) $(TEST_CHUNK) $(NODE_TSAN)

# -----------------------------
# Convenience rules
# -----------------------------
run-tracker:
	./$(TRACKER) 8000

run-client:
	./$(CLIENT) 127.0.0.1 8000 6881

# Build and run the in-process arithmetic tests. Exits non-zero on failure, so
# it can be used as a gate.
check: $(TEST_CHORD) $(TEST_NET) $(TEST_LOOKUP) $(TEST_STABILIZE) $(TEST_CHUNK) $(TEST_TRANSFER) $(TEST_NAMES)
	./$(TEST_CHORD)
	./$(TEST_NET)
	./$(TEST_LOOKUP)
	./$(TEST_STABILIZE)
	./$(TEST_CHUNK)
	./$(TEST_TRANSFER)
	./$(TEST_NAMES)

.PHONY: all clean run-tracker run-client check tsan
