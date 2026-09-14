# cowsay.wasm

A C reimplementation of [cowsay 3.03](https://github.com/tnalpgge/rank-amateur-cowsay) (c) 1999-2000 Tony Monroe, written for compilation to `wasm32-wasip1`.

The original cowsay is a Perl script; the popular wasm build on the Wasmer registry is a Rust clone that weighs 772 kB and mis-draws the cow.
This implementation produces the original's exact output from a single C file, and the wasm binary is about 68 kB with all 47 cowfiles embedded.

```console
$ echo moo | wasmtime run cowsay.wasm
 _____ 
< moo >
 ----- 
        \   ^__^
         \  (oo)\_______
            (__)\       )\/\
                ||----w |
                ||     ||
```

## Output contract

For ASCII input, stdout, stderr and the exit code are byte-identical to cowsay 3.03 running under a modern Perl (Text::Wrap 2018.6 or later), except for the deliberate fixes listed below.
The test suite (`make check`) enforces this differentially: every case runs both this implementation and the vendored reference script (`test/reference/cowsay`) under the host Perl and compares all three channels, including 250 deterministic fuzz cases.
The fixed behaviors are pinned by golden files under `test/fixed/` instead.

The remaining quirks of the original are part of the contract and are reproduced deliberately, among them:

- `-l` wraps the cowfile list at 76 columns regardless of `-W`, because the original lists before applying `-W`.
- A missing cowfile exits with status 2, the `ENOENT` that Perl's `die` picks up from the failed file test.
- Face flags override each other in a fixed order (`-y -b` shows `==`), and any of them overrides `-e`/`-T`.
- An unknown option warns and parsing continues.

Exit codes: 0 on success, 1 for a rejected cowfile, 2 for a missing cowfile, 64 for a usage error; all representable under WASI preview 1's [0..126) restriction.

### Deliberate fixes

Four behaviors of the reference are bugs with no value to preserve and are fixed here:

- `-W` below 2 behaves as a width of 2. The reference interpolates a negative regex quantifier into Text::Wrap, which then returns its argument count: the message becomes `3`.
- Any remaining argument selects the argument message. The reference tests `unless ($ARGV[0])`, so `cowsay 0` and `cowsay ""` silently wait on stdin instead.
- `-f` with a missing path containing `/` reports `Could not find ... cowfile!` and exits 2. The reference runs `do $full` without checking existence and prints the balloon with no cow, exiting 0.
- A usage error exits with EX_USAGE (64) instead of the reference's 255, which WASI preview 1 cannot represent, and the usage text drops a stray trailing space carried over from the reference's heredoc.

### Out of the contract

- `--help` and `--version`, whose Getopt::Std output embeds the host Perl version.
- Malformed cowfiles: the reference reports a Perl error, this implementation reports its own and exits 1 (see "Cowfiles").
- `-W` values that are not a decimal integer with optional sign; the leading integer prefix is used.

## UTF-8 behavior

The reference counts bytes and can split a multi-byte character when breaking an overlong word.
This implementation counts Unicode codepoints instead and never splits a UTF-8 sequence:

- Balloon padding and wrap positions count one column per codepoint.
- Breaking an overlong word happens at a codepoint boundary.
- `-e` and `-T` truncate to two codepoints, not two bytes.
- A byte that does not form a valid UTF-8 sequence counts as one column and passes through unchanged, so arbitrary byte input still works.

Grapheme clusters and East Asian width are intentionally out of scope: a combining mark or a double-width character still counts as one column.
ASCII input is unaffected, since there a byte and a codepoint are the same thing.

## Cowfiles

All 47 `.cow` files from cowsay 3.03 are embedded in the binary, so `-f name` and `-l` work without any filesystem access.
Setting `COWPATH` switches to real directories with the original search rules (`dir/name`, then `dir/name.cow`); under a wasm runtime those need a preopen, e.g. `wasmtime run --dir cows --env COWPATH=cows cowsay.wasm -f moose`.
A `-f` value containing `/` always reads the real filesystem.

A cowfile is a Perl script, but every file shipped with cowsay 3.03 fits a small grammar: comments, one interpolating heredoc assigned to `$the_cow`, and the two eye-manipulation statement idioms used by `small.cow`, `three-eyes.cow` and `udder.cow`.
The parser accepts exactly that grammar and rejects anything else with an error naming the line, so a cowfile is either rendered byte-identically or refused, never mis-rendered.
`mech-and-cow` (not a valid cowfile even for the original) and the `*.pm` modules from the upstream `cows/` directory are not included.

## cowthink

Like the original, thought bubbles are selected by the program name: an invocation path containing `think` (case-insensitive) switches to `cowthink`.

```console
$ cp cowsay.wasm cowthink.wasm && echo moo | wasmtime run cowthink.wasm
```

## Building

```console
$ make                # cowsay.wasm, needs WASI_SDK_PATH (wasi-sdk-34) and optionally wasm-opt
$ make cowsay-native  # host binary, same behavior, used by the tests
$ make check          # differential test suite against the vendored reference
$ COWSAY_TEST_MODE=wasm bash test/run.sh   # the same suite under wasmtime
```

## License

GPL-3.0-only, the license of cowsay 3.03; the cowfiles under `cows/` and the reference script under `test/reference/` are taken from it unmodified.
