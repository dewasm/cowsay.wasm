# wasi-sdk root; local dev and CI use wasi-sdk-34.
WASI_SDK_PATH ?= /opt/wasi-sdk

CFLAGS ?= -Wall -Wextra -std=c99
WASM_CFLAGS = $(CFLAGS) -Oz -flto -Wl,--strip-all

COWS = $(sort $(wildcard cows/*.cow))

.PHONY: all check check-native check-wasm lint clean

all: cowsay.wasm

cows_embedded.h: tools/embed-cows.sh $(COWS)
	sh tools/embed-cows.sh $(COWS) > $@

cowsay.wasm: cowsay.c cows_embedded.h
	$(WASI_SDK_PATH)/bin/clang --target=wasm32-wasip1 $(WASM_CFLAGS) -o $@ cowsay.c
	@command -v wasm-opt >/dev/null && { wasm-opt -Oz -o $@.opt $@ && mv $@.opt $@; } || true
	@ls -l $@

cowsay-native: cowsay.c cows_embedded.h
	$(CC) $(CFLAGS) -O2 -o $@ cowsay.c

check: check-native check-wasm

check-native: cowsay-native
	@bash test/run.sh

check-wasm: cowsay.wasm
	@COWSAY_TEST_MODE=wasm bash test/run.sh

# The README picture of clawd, whose colors a code block cannot show.
docs/clawd.svg: cowsay-native tools/ansi-to-svg.pl cows/clawd.cow
	./cowsay-native -f clawd "Hello from cowsay.wasm" \
	  | perl tools/ansi-to-svg.pl '$$ cowsay -f clawd "Hello from cowsay.wasm"' > $@

lint:
	shellcheck test/run.sh tools/embed-cows.sh
	perl -c test/gen-fuzz.pl
	perl -c tools/ansi-to-svg.pl

clean:
	rm -f cowsay.wasm cowsay-native cowthink-native cows_embedded.h
