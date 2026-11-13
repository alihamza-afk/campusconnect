# ---------------------------------------------------------------------------
# CampusConnect - build, test, run.
#
#   make          build (downloads the two library headers the first time)
#   make run      build and start the server on http://localhost:8080
#   make test     build and run the unit tests
#   make asan     build with AddressSanitizer + UBSan (slow, finds memory bugs)
#   make clean    delete the objects and binaries
#
# Every .cpp file is compiled to its own .o in build/, then linked together.
# That is what the .h/.cpp split buys you: change store.cpp and only store.o
# (plus the final link) is rebuilt, not the whole program.
# ---------------------------------------------------------------------------
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

# pkg-config knows where Homebrew puts libsodium; fall back to a plain -l flag.
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null)
ifeq ($(strip $(SODIUM_LIBS)),)
SODIUM_LIBS := -lsodium
endif
DB_LIBS := -lsqlite3
CURL_LIBS := -lcurl

BUILD    := build
TARGET   := backend/campusconnect
TEST_BIN := tests/test_units

SRCS := backend/main.cpp \
        backend/db.cpp \
        backend/schema.cpp \
        backend/user_repo.cpp \
        backend/item_repo.cpp \
        backend/legacy_json.cpp \
        backend/store.cpp \
        backend/auth.cpp \
        backend/secrets.cpp \
        backend/password.cpp \
        backend/mailer.cpp \
        backend/email_token_repo.cpp \
        backend/email_service.cpp \
        backend/rate_limiter.cpp \
        backend/claim_repo.cpp \
        backend/text.cpp \
        backend/inverted_index.cpp \
        backend/session_repo.cpp \
        backend/hashtable.cpp \
        backend/trie.cpp \
        backend/mail_queue.cpp \
        backend/images.cpp \
        backend/notification_repo.cpp
OBJS     := $(patsubst backend/%.cpp,$(BUILD)/%.o,$(SRCS))
LIB_OBJS := $(filter-out $(BUILD)/main.o,$(OBJS))

HDRS := $(wildcard backend/*.h)
DEPS := backend/httplib.h backend/json.hpp

.PHONY: all deps run test asan clean

all: $(TARGET)

deps: $(DEPS)

$(DEPS):
	sh scripts/fetch_deps.sh backend

$(BUILD)/%.o: backend/%.cpp $(HDRS) $(DEPS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SODIUM_CFLAGS) -pthread -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -pthread -o $(TARGET) $(OBJS) $(SODIUM_LIBS) $(DB_LIBS) $(CURL_LIBS)

run: $(TARGET)
	cd backend && ./campusconnect

test: $(DEPS) $(LIB_OBJS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SODIUM_CFLAGS) -pthread -Ibackend -c tests/test_units.cpp -o $(BUILD)/test_units.o
	$(CXX) $(CXXFLAGS) -pthread -o $(TEST_BIN) $(BUILD)/test_units.o $(LIB_OBJS) $(SODIUM_LIBS) $(DB_LIBS) $(CURL_LIBS)
	./$(TEST_BIN)

asan: $(DEPS)
	$(CXX) -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -pthread $(SODIUM_CFLAGS) -o backend/campusconnect_asan $(SRCS) $(SODIUM_LIBS) $(DB_LIBS) $(CURL_LIBS)

clean:
	rm -rf $(BUILD) $(TARGET) backend/campusconnect_asan backend/campusconnect_asan.dSYM $(TEST_BIN)
