# wasi-sdk root; local dev and CI use wasi-sdk-34.
WASI_SDK_PATH ?= /opt/wasi-sdk

CFLAGS ?= -Wall -Wextra -std=c99
WASM_CFLAGS = $(CFLAGS) -Oz -flto -Wl,--strip-all

COWS = $(sort $(wildcard cows/*.cow))
SRC = cowsay.c width.c

# The version tools/fetch-ucd.sh pulls into ucd/; the tables follow what is vendored there.
UNICODE_VERSION = 18.0.0
UCD = ucd
UCD_FILES = $(UCD)/EastAsianWidth.txt $(UCD)/DerivedCoreProperties.txt \
            $(UCD)/DerivedGeneralCategory.txt $(UCD)/emoji-data.txt \
            $(UCD)/GraphemeBreakProperty.txt

.PHONY: all check check-width check-native check-wasm lint update-ucd clean

all: cowsay.wasm

cows_embedded.h: tools/embed-cows.sh $(COWS)
	sh tools/embed-cows.sh $(COWS) > $@

cowsay.wasm: $(SRC) cows_embedded.h unicode_tables.h width.h
	$(WASI_SDK_PATH)/bin/clang --target=wasm32-wasip1 $(WASM_CFLAGS) -o $@ $(SRC)
	@command -v wasm-opt >/dev/null && { wasm-opt -Oz -o $@.opt $@ && mv $@.opt $@; } || true
	@ls -l $@

cowsay-native: $(SRC) cows_embedded.h unicode_tables.h width.h
	$(CC) $(CFLAGS) -O2 -o $@ $(SRC)

width-test: test/width-test.c width.c unicode_tables.h width.h $(UCD)/GraphemeBreakTest.txt
	$(CC) $(CFLAGS) -O1 -I. -o $@ test/width-test.c width.c

check: check-width check-native check-wasm

# The Unicode side: the UCD's own break test, plus the width and rendition rules cowsay relies on.
check-width: width-test
	@./width-test

check-native: cowsay-native
	@bash test/run.sh

check-wasm: cowsay.wasm
	@COWSAY_TEST_MODE=wasm bash test/run.sh

# The README picture of clawd, whose colors a code block cannot show.
docs/clawd.svg: cowsay-native tools/ansi-to-svg.pl cows/clawd.cow
	./cowsay-native -f clawd "Hello from cowsay.wasm" \
	  | perl tools/ansi-to-svg.pl '$$ cowsay -f clawd "Hello from cowsay.wasm"' > $@

# The tables are built from the vendored UCD, like cows_embedded.h is built from cows/.
unicode_tables.h: tools/gen-unicode-tables.pl $(UCD_FILES)
	perl tools/gen-unicode-tables.pl $(UCD) > $@

# The only networked step: move the vendored UCD to UNICODE_VERSION, then commit the diff.
update-ucd:
	sh tools/fetch-ucd.sh $(UNICODE_VERSION) $(UCD)

lint:
	shellcheck test/run.sh tools/embed-cows.sh
	perl -c test/gen-fuzz.pl
	perl -c tools/ansi-to-svg.pl
	perl -c tools/gen-unicode-tables.pl
	shellcheck tools/fetch-ucd.sh

clean:
	rm -f cowsay.wasm cowsay-native cowthink-native width-test cows_embedded.h unicode_tables.h
