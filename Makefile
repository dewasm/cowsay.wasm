# wasi-sdk root; local dev and CI use wasi-sdk-34.
WASI_SDK_PATH ?= /opt/wasi-sdk

CFLAGS ?= -Wall -Wextra -std=c99
WASM_CFLAGS = $(CFLAGS) -Oz -flto -Wl,--strip-all

COWS = $(sort $(wildcard cows/*.cow))

.PHONY: all check clean

all: cowsay.wasm

cows_embedded.h: tools/embed-cows.sh $(COWS)
	sh tools/embed-cows.sh $(COWS) > $@

cowsay.wasm: cowsay.c cows_embedded.h
	$(WASI_SDK_PATH)/bin/clang --target=wasm32-wasip1 $(WASM_CFLAGS) -o $@ cowsay.c
	@command -v wasm-opt >/dev/null && { wasm-opt -Oz -o $@.opt $@ && mv $@.opt $@; } || true
	@ls -l $@

cowsay-native: cowsay.c cows_embedded.h
	$(CC) $(CFLAGS) -O2 -o $@ cowsay.c

check: cowsay-native
	bash test/run.sh

clean:
	rm -f cowsay.wasm cowsay-native cowthink-native cows_embedded.h
