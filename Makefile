CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
BUILD   := build

.PHONY: all test clean

all: $(BUILD)/libnre.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/nre.o: src/nre.c include/nre.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libnre.a: $(BUILD)/nre.o
	$(AR) rcs $@ $^

$(BUILD)/test_nre: tests/test_nre.c $(BUILD)/libnre.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libnre.a -o $@

test: $(BUILD)/test_nre
	./$(BUILD)/test_nre

clean:
	rm -rf $(BUILD)
