# -----------------------------
# Makefile for P2P File Sharing
# -----------------------------

# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2

# Executables
TRACKER = tracker
CLIENT  = client

# Source files
TRACKER_SRC = tracker.cpp sha1.cpp
CLIENT_SRC  = client.cpp sha1.cpp

# Object files
TRACKER_OBJ = $(TRACKER_SRC:.cpp=.o)
CLIENT_OBJ  = $(CLIENT_SRC:.cpp=.o)

# Default target
all: $(TRACKER) $(CLIENT)

# -----------------------------
# Build rules
# -----------------------------

$(TRACKER): $(TRACKER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(CLIENT): $(CLIENT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# -----------------------------
# Clean rule
# -----------------------------
clean:
	rm -f $(TRACKER_OBJ) $(CLIENT_OBJ) $(TRACKER) $(CLIENT)

# -----------------------------
# Convenience rules
# -----------------------------
run-tracker:
	./$(TRACKER) 8000

run-client:
	./$(CLIENT) 127.0.0.1 8000 6881
