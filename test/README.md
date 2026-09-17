# Specification and test suite

The behavior is defined against [cowsay 3.03](https://github.com/tnalpgge/rank-amateur-cowsay).
Its script is vendored here as `reference/cowsay`, where it runs under the host Perl.
This file states the specification, while `run.sh` enforces it.

## Test suite

Run it with `make check`, or one mode at a time with `make check-native` and `make check-wasm`.
`COWSAY_TEST_MODE=wasm` selects `cowsay.wasm` under wasmtime; the default runs `cowsay-native`.

Each case compares all three channels against the reference.
To do that it runs this implementation twice: with `COWPATH` pointing at `cows/`, then without it.
Those two runs cover the real-filesystem lookup and the embedded cowfiles respectively.
A case whose output names the cowfile directory, such as `-l`, skips the embedded run.

| Path | What it holds |
| --- | --- |
| `reference/cowsay` | cowsay 3.03 unmodified, the reference every differential case runs against |
| `fixed/` | snapshots of the deliberate fixes, which diverge from the reference on purpose |
| `utf8/` | snapshots of the UTF-8 behavior, which the byte-based reference cannot define |
| `gen-fuzz.pl` | 250 deterministic fuzz cases (`srand(42)`): 150 from arguments, 100 from stdin |

The fuzz generator stays inside the specification.
It therefore emits neither a width below 2 nor a first message word of `0`;
those are deliberate fixes, and the snapshot cases already cover them.

## Output specification

For ASCII input, stdout, stderr and the exit code are identical to the reference.
That reference runs under a modern Perl, meaning `Text::Wrap` 2018.6 or later.
The only exceptions are the deliberate fixes below, which snapshot files under `fixed/` pin instead.

The remaining behaviors of the original are part of the specification too, reproduced deliberately:

- `-l` wraps the cowfile list at 76 columns, because the original lists before applying `-W`.
- A missing cowfile exits with status 2, the `ENOENT` that Perl's `die` picks up from the file test.
- Face flags override each other in a fixed order (`-y -b` shows `==`), and all override `-e`/`-T`.
- An unknown option warns and parsing continues.

Exit codes: 0 on success, 1 for a rejected cowfile, 2 for a missing cowfile, 64 for a usage error.
All of them are representable under WASI preview 1's [0..126) restriction.

### Deliberate fixes

The following behaviors of the reference are bugs with no value to preserve and are fixed here:

- `-W` below 2 behaves as a width of 2.
  The reference instead feeds `Text::Wrap` a negative regex quantifier;
  it then returns its argument count, so that the message becomes `3`.
- Any remaining argument selects the argument message.
  The reference instead tests `unless ($ARGV[0])`, so `cowsay 0` and `cowsay ""` wait on stdin.
- `-f` with a missing path containing `/` reports `Could not find ... cowfile!` and exits 2.
  The reference instead runs `do $full` unchecked, prints the balloon with no cow, and exits 0.
- A usage error exits with `EX_USAGE` (64) rather than the reference's 255;
  WASI preview 1 cannot represent a status that high.
  The usage text also drops a stray trailing space, carried over from the reference's heredoc.

### Outside the specification

- `--help` and `--version`, whose Getopt::Std output embeds the host Perl version.
- Malformed cowfiles: the reference reports a Perl error; this implementation reports its own.
- `-W` values that are not a decimal integer with optional sign; the leading integer prefix is used.

## UTF-8 behavior

The reference counts bytes and can split a multi-byte character when breaking an overlong word.
This implementation counts Unicode codepoints instead and never splits a UTF-8 sequence:

- Balloon padding and wrap positions count one column per codepoint.
- Breaking an overlong word happens at a codepoint boundary.
- `-e` and `-T` truncate to two codepoints, not two bytes.
- `chop` and `substr` in a cowfile take a whole codepoint as well.
- A byte that forms no valid UTF-8 sequence counts as one column, and passes through unchanged.
  Arbitrary byte input therefore still works.

Grapheme clusters and East Asian width are intentionally out of scope.
A combining mark or a double-width character therefore still counts as one column.
ASCII input is unaffected either way, since there a byte and a codepoint are the same thing.

## Cowfile grammar

A cowfile is a Perl script, and the original simply runs it with `do`.
This implementation reads a restricted grammar of it instead, and refuses whatever falls outside.

A file is comment lines and blank ones, then the assignments below, then a single heredoc:

```perl
$the_cow = <<EOC;
```

The terminator may be quoted as `<<"EOC"`, and the semicolon may be left out, as `sheep.cow` does.
Once the terminator line closes the heredoc, only comments and blank lines may follow.

The assignments before it are the eye idioms that the shipped cowfiles use:

- `$var = chop($eyes);` moves the last character of `$eyes` into a variable of any other name.
- `$var = substr($eyes, 0, 1);` copies the character at that position instead, as `clawd.cow` does.
- `$eyes .= ($var x 2);` appends that character twice, as `three-eyes.cow` does.
- `$eyes .= " $var";` appends it after one or more spaces, as `udder.cow` does with one.
- `$eyes .= "  ";` appends a literal, which `clawd.cow` uses to pad `$eyes` out to two characters.
- `$eyes = "..." unless ($eyes);` fills in a default, as `small.cow` does.
- `$eyes = "..." if ($eyes eq "...");` replaces one value with another;
  `clawd.cow` blanks the default `oo` that way, so its eye cells stay plain until `-e` fills them.

Inside the heredoc body:

- `$thoughts`, `$eyes`, `$tongue`, and any variable from the assignments above, interpolate.
  The `${name}` form works as well, while a `$` that no name follows stays literal.
- The escapes are `\\`, `\$`, `\@` and `\e`, that last one being the ESC `clawd.cow` colors with.
- A bare `@name` is refused rather than interpolated, and so is an unknown `$name`.
