CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
TARGET = warship

# proto.o is the only network-adjacent object testable without sockets, so it is
# the only one linked into the unit tests.
GAME_OBJ = board.o proto.o
NET_OBJ = signaling.o net.o
OBJ = $(GAME_OBJ) ui.o $(NET_OBJ) main.o

# libjuice ships CMake config but no .pc file, so there is nothing for
# pkg-config to find; the nix dev shell puts its -I/-L on the cc wrapper.
LDLIBS = -ljuice -lplum -lwebsockets

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

test_signaling: signaling.o test_signaling.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_net: $(NET_OBJ) test_net.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Needs a live server, so it stays out of `make test`.
itest: signal-server test_signaling test_net
	@# A per-run port. Back-to-back runs otherwise collide on a socket the
	@# previous server has not finished releasing, which looks like a flaky test.
	@port=$$((18000 + $$$$ % 1000)); \
	  ./signal-server $$port >/dev/null 2>&1 & pid=$$!; sleep 0.6; \
	  port=$$((18000 + $$$$ % 1000)); \
	  ./test_signaling $$port && ./test_net $$port && ./test_net $$port nopath; \
	  rc=$$?; \
	  kill $$pid 2>/dev/null; wait $$pid 2>/dev/null; exit $$rc

# Regression test against the real pochi.casa deployment.
prod-check: test_signaling test_net
	@./tools/infra-check.sh

# Drives two real processes on ptys through a full turn. Needs python3.
smoke: $(TARGET) signal-server
	@python3 tools/smoke.py

signal-server: server/signaling_server.c
	$(CC) $(CFLAGS) -o $@ $< -lwebsockets

TESTS = test_board test_proto

test: $(TESTS) test_net
	@for t in $(TESTS); do ./$$t || exit 1; done
	@./test_net unit

test_board: board.o test_board.o
	$(CC) $(CFLAGS) -o $@ $^

test_proto: board.o proto.o test_proto.o
	$(CC) $(CFLAGS) -o $@ $^

# Blunt but correct: any header change rebuilds everything. A handful of files.
%.o: %.c board.h ui.h proto.h signaling.h net.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(TESTS) test_signaling test_net signal-server *.o

.PHONY: clean test itest smoke prod-check
