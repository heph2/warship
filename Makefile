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
LDLIBS = -ljuice

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

test_signaling: signaling.o test_signaling.o
	$(CC) $(CFLAGS) -o $@ $^

test_net: $(NET_OBJ) test_net.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Needs a live server, so it stays out of `make test`.
itest: rendezvous test_signaling test_net
	@./rendezvous 17778 & pid=$$!; sleep 0.3; \
	  ./test_signaling 17778 && ./test_net 17778 && ./test_net 17778 relay; \
	  rc=$$?; \
	  kill $$pid 2>/dev/null; wait $$pid 2>/dev/null; exit $$rc

# Drives two real processes on ptys through a full turn. Needs python3.
smoke: $(TARGET) rendezvous
	@python3 tools/smoke.py

rendezvous: server/rendezvous.c
	$(CC) $(CFLAGS) -o $@ $<

TESTS = test_board test_proto

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_board: board.o test_board.o
	$(CC) $(CFLAGS) -o $@ $^

test_proto: board.o proto.o test_proto.o
	$(CC) $(CFLAGS) -o $@ $^

# Blunt but correct: any header change rebuilds everything. A handful of files.
%.o: %.c board.h ui.h proto.h signaling.h net.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(TESTS) test_signaling test_net rendezvous *.o

.PHONY: clean test itest smoke
