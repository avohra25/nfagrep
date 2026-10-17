CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
BUILD   := build
PREFIX  ?= /usr/local

.PHONY: all test clean install

all: $(BUILD)/libnre.a $(BUILD)/nfagrep

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/nre.o: src/nre.c include/nre.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libnre.a: $(BUILD)/nre.o
	$(AR) rcs $@ $^

$(BUILD)/nfagrep: src/nfagrep.c $(BUILD)/libnre.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libnre.a -o $@

$(BUILD)/test_nre: tests/test_nre.c $(BUILD)/libnre.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libnre.a -o $@

test: $(BUILD)/test_nre $(BUILD)/nfagrep
	./$(BUILD)/test_nre
	sh tests/cli_test.sh ./$(BUILD)/nfagrep

install: $(BUILD)/nfagrep
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/nfagrep $(DESTDIR)$(PREFIX)/bin/nfagrep

clean:
	rm -rf $(BUILD)
