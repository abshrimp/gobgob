CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra -std=c11 -march=native -pthread
LDFLAGS ?= -pthread

OBJ_COMMON = position.o moves.o

all: analyze query test_pos

analyze: analyze.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

query: query.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_pos: test_pos.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c gobblet.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o analyze query test_pos

.PHONY: all clean
