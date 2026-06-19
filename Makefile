CC := gcc
CFLAGS := -O3 -march=native -fno-tree-vectorize -fno-omit-frame-pointer -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS :=

.PHONY: all clean

all: benchmark

benchmark: benchmark.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f benchmark
