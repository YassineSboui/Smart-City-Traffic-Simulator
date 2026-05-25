CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE
LDFLAGS ?= -pthread

TARGET := smartcross
SRC := src/main.c src/common.c src/route.c src/simulation.c src/benchmark.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean run benchmark

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c src/smartcross.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET) -mode normal -strategy rr -cars 80 -p 5 -t 2

benchmark: $(TARGET)
	./$(TARGET) -mode benchmark -cars 120 -p 5 -t 4 -quiet

clean:
	rm -f $(TARGET) $(OBJ) results.csv benchmark.png benchmark.txt smartcross_live.txt smartcross_live.tmp smartcross_events.txt
