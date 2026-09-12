# -----------------------------
# Makefile for P2P File Sharing
# -----------------------------

# Compiler and flags
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2

# Link-time libraries.
# client.cpp includes sha1.h, which is a header-only wrapper over OpenSSL's SHA1().
# The declaration comes from <openssl/sha.h> at compile time, but the machine code for
# SHA1 lives in libcrypto, so it must be named at LINK time or the linker reports
# "undefined reference to SHA1". tracker.cpp does not hash anything today, so it does
# not need it (defect B2 in PROGRESS.md; see docs/failures.md).
CRYPTO_LDLIBS = -lcrypto

# Executables
TRACKER = tracker
CLIENT  = client

# test-chord is a TEST binary, not a deliverable: it exercises the identifier
# arithmetic in process, with rings built by hand. It is deliberately not part
# of `all`, so a normal build never depends on it.
TEST_CHORD = test-chord

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

# Object files
TRACKER_OBJ    = $(TRACKER_SRC:.cpp=.o)
CLIENT_OBJ     = $(CLIENT_SRC:.cpp=.o)
CHORD_OBJ      = $(CHORD_SRC:.cpp=.o)
TEST_CHORD_OBJ = $(TEST_CHORD_SRC:.cpp=.o)

# Default target
all: $(TRACKER) $(CLIENT)

# -----------------------------
# Build rules
# -----------------------------

$(TRACKER): $(TRACKER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(CLIENT): $(CLIENT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

$(TEST_CHORD): $(TEST_CHORD_OBJ) $(CHORD_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(CRYPTO_LDLIBS)

# Rebuild any object file if sha1.h changes. Without this, editing the header leaves
# stale .o files behind and the next `make` links yesterday's code.
$(CLIENT_OBJ): sha1.h
$(CHORD_OBJ): chord.h sha1.h
$(TEST_CHORD_OBJ): chord.h

# -----------------------------
# Clean rule
# -----------------------------
clean:
	rm -f $(TRACKER_OBJ) $(CLIENT_OBJ) $(CHORD_OBJ) $(TEST_CHORD_OBJ) \
	      $(TRACKER) $(CLIENT) $(TEST_CHORD)

# -----------------------------
# Convenience rules
# -----------------------------
run-tracker:
	./$(TRACKER) 8000

run-client:
	./$(CLIENT) 127.0.0.1 8000 6881

# Build and run the in-process arithmetic tests. Exits non-zero on failure, so
# it can be used as a gate.
check: $(TEST_CHORD)
	./$(TEST_CHORD)

.PHONY: all clean run-tracker run-client check
