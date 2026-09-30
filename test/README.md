# Specification and test suite

The behavior is defined against [cowsay 3.8.4](https://github.com/cowsay-org/cowsay), the reference.
Its script is `bin/cowsay` of the submodule `submodules/cowsay-org-cowsay`, run under the host Perl.
This file states the specification, while `run.sh` enforces it.

## Test suite

Run it with `make check`, or one mode at a time with `make check-native` and `make check-wasm`.
`COWSAY_TEST_MODE=wasm` selects `cowsay.wasm` under wasmtime; the default is `cowsay-native`.

Each case compares stdout, stderr and the exit code against the reference.
It runs our binary twice: with `COWPATH` set to `cows/`, then without it.
The first run reads cowfiles from the filesystem, and the second uses the embedded ones.
A case that reads a cowfile outside `cows/` skips the embedded run.

The suite clears `LANG`, `LC_ALL`, `LC_CTYPE` and `COWSAY_AMBIGUOUS_WIDTH`.
A wasm run sees no environment, so a native run must not read the machine's.
A width case sets the ones it tests.

Fuzz cases avoid the intended fixes, where the output differs from the reference:
a width below 2, and a first message word `0`.
The snapshots cover those.

`submodules/` holds repositories of cowfiles as submodules, each at a fixed commit.
The suite also tests some of their cowfiles, and each one must match the reference or be refused.
The reference runs a cowfile with Perl `do`, so only cowfiles our parser accepts reach it.

The suite reads these files, with paths relative to `test/`:

| Path | What it holds |
| --- | --- |
| `fixed/` | snapshots of the intended fixes, which differ from the reference |
| `width/` | snapshots of non-ASCII width, which the byte-based reference cannot define |
| `gen-fuzz.pl` | 250 deterministic fuzz cases (`srand(42)`): 150 from arguments, 100 from stdin |
| `width-test.c` | the UCD's break test, plus the width and rendition rules (`make check-width`) |
| `../ucd/` | the UCD files as published, read for the tables and the break test |
| `submodules/` | repositories of cowfiles, each a submodule at a fixed commit, the reference among them |

## Output specification

For ASCII input, stdout, stderr and the exit code are identical to the reference.
That reference runs under a modern Perl, meaning `Text::Wrap` 2018.6 or later.
The only exceptions are the intended fixes below, which snapshot files under `fixed/` pin instead.

The remaining behaviors of the original are part of the specification too, reproduced intentionally:

- `-l` prints the cowfile names alone, one per line, when stdout is no terminal.
  On a terminal it lists each cowpath directory that holds a cowfile, and skips the others.
  That list wraps at 76 columns, since the reference lists before it applies `-W`.
- `-f` reads a file of that name if there is one.
  Otherwise it searches the cowpath: the built-in cowfiles, then each directory of `COWPATH`.
  `COWSAY_ONLY_COWPATH`, when it equals 1 as a Perl number, leaves the built-in cowfiles out.
- A missing cowfile exits with status 2, the `ENOENT` that Perl's `die` picks up from the file test.
- `-n` with a message on the command line prints the help and exits with status 1.
- Face flags override each other in a fixed order (`-y -b` shows `==`), and all override `-e`/`-T`.
- An unknown option warns and parsing continues.

Exit codes: 0 on success and for `-h`, 1 for a rejected cowfile and for `-n` with a message,
2 for a missing cowfile.
All of them are representable under WASI preview 1's [0..126) restriction.

### Intended fixes

The following behaviors of the reference are bugs with no value to preserve and are fixed here:

- `-W` below 2 behaves as a width of 2.
  The reference instead feeds `Text::Wrap` a negative regex quantifier;
  it then returns its argument count, so that the message becomes `3`.
- Any remaining argument selects the argument message.
  The reference instead tests `unless ($ARGV[0])`, and Perl takes `"0"` and `""` as false,
  so `cowsay 0` and `cowsay ""` wait on stdin.
- A cowfile's `($eyes)` is false only for an empty `$eyes`.
  Perl takes `"0"` as false too, so the reference shows the default `..` for `cowsay -e 0 -f small`.
- The help names this build beside the cowsay it implements: `version 3.8.4 (cowsay.wasm 0.2.0)`.
  It also leaves out `-r` and `-C`, which this build does not have.
- A relative path in `-f`, such as `-f cows/tux.cow`, reads that file.
  The reference loads it with `do`, which searches `@INC` unless the path starts with `./` or `../`,
  so it prints the balloon with no cow.
- An ANSI escape sequence counts as no columns, and an open colour carries across a wrapped line.
  The reference counts those bytes as text, so the balloon widens by the length of the sequence;
  the rest of the message also loses its colour at the first break.
- A cow with a character above U+00FF prints with no warning.
  Perl also prints `Wide character in print` on stderr for it.

### Outside the specification

- `--help` and `--version`, whose Getopt::Std output embeds the host Perl version.
- `-r` and `-C`, which this build does not have.
- A subdirectory of a cowpath directory, whose cowfiles the reference names as `dir/name`.
- The built-in cowfiles in the `-l` list on a terminal: the reference names its own directory there.
- A `COWSAY_ONLY_COWPATH` that only rounding makes 1, such as `1.0000000000000000001`.
- Malformed cowfiles: Perl reports its own error, and so does this implementation.
- `-W` values that are not a decimal integer with optional sign; the leading integer prefix is used.

## Display width

The reference counts bytes, so a combining mark widens the balloon and a CJK character narrows it,
and an overlong word breaks in the middle of a character.
This implementation counts the columns a terminal uses, per Unicode 18.0.0:

The property tables come from the UCD files vendored under `ucd/`, so a build stays offline.
`make update-ucd` moves those to another Unicode version, and the tables follow.

- Text is measured in **extended grapheme clusters** (UAX #29), so a line never breaks inside one.
- A cluster takes the width of its base character: two columns for East Asian Wide and Fullwidth,
  zero for a combining mark or another zero-width character, one otherwise.
- The emoji rules override that: a cluster carrying U+FE0F takes two columns and U+FE0E one,
  while a regional indicator pair, or a pictograph with emoji presentation, takes two.
- `-e` and `-T` truncate to two clusters, so a mark stays with the character it belongs to.
- A tab under `-n` runs to the next multiple of eight columns rather than of eight characters.
- A byte that forms no valid UTF-8 sequence counts as one column and passes through unchanged,
  so arbitrary byte input still works.
- `chop` and `substr` in a cowfile still take one codepoint, matching what Perl does to `$eyes`.

ASCII input is unaffected, since there a byte, a codepoint and a column are the same thing.

### East Asian Ambiguous

An Ambiguous character takes one column, which is Unicode's default and what a terminal assumes.
Two settings widen it to two, the first that applies winning:

1. `COWSAY_AMBIGUOUS_WIDTH=2` (or `=1` to keep it narrow) settles it outright.
2. `LC_ALL`, `LC_CTYPE` or `LANG`, in that order, naming a `ja`, `ko` or `zh` locale.

A wasm runtime hands the guest no environment unless asked,
so under `wasmtime run` the default holds until `--env` passes one in.

### ANSI escape sequences

An escape sequence occupies no columns, so a coloured message wraps by what it shows.
CSI and OSC sequences are recognized; the parameters of anything else pass through untouched.

A colour still open when a line wraps is reopened on the next line and closed at its end.
It therefore runs down the balloon, while the frame and the padding keep the terminal's own colours.
Foreground, background, and the bold, dim, italic, underline, reverse and strikethrough attributes
carry across; anything else passes through without being carried.

## Cowfile grammar

A cowfile is a Perl script, and the original simply runs it with `do`.
This implementation reads a restricted grammar of it instead, and refuses whatever falls outside.

A file is comment lines and blank ones, then the assignments below, then a single heredoc:

```perl
$the_cow = <<EOC;
```

The terminator may be quoted as `<<"EOC"`, and the semicolon may be left out, as `sheep.cow` does.
Under `<<'EOC'` the body is text as it stands: nothing interpolates, and a backslash is text too.
A space may follow `<<` only before a quoted terminator, as in Perl.
Once the terminator line closes the heredoc, only comments and blank lines may follow.
A `#` comment may end any statement, the heredoc line included.
A CR before an LF is dropped anywhere in the file, as Perl drops it, so CRLF line ends read as LF.

The statements before it are `use utf8;` and the assignments below, each on one line.

- `$var = "...";` sets a variable of any name but `the_cow`, as converted cowfiles do:
  `$x = "\e[49m  ";` and `$t = "$thoughts ";`.
  Setting `$eyes`, `$tongue` or `$thoughts` changes what the heredoc reads.
- `$var .= "...";` appends to a variable that has a value.
- `$var = chop($eyes);` moves the last character of `$eyes` into a variable of any other name.
- `$var = substr($eyes, 0, 1);` copies the character at that position instead, as `clawd.cow` does.
- `$var .= ($other x 2);` appends a variable twice, as `three-eyes.cow` does to `$eyes`.
- `$eyes = "..." unless ($eyes);` fills in a default, as `small.cow` does.
  Only an empty `$eyes` is false here, one of the intended fixes.
- `$eyes = "..." if ($eyes eq "...");` replaces one value with another;
  `clawd.cow` blanks the default `oo` that way, so its eye cells stay plain until `-e` fills them.
  `if` and `unless` each take `($eyes)`, `($eyes eq "...")` or `($eyes ne "...")`.

A literal `"..."` follows the rules of the heredoc body below, with `\"` for a quote.
Perl can run code from inside one, as in `"@{[ ... ]}"`, and those rules refuse every such form.
A literal `'...'` interpolates nothing, and its only escapes are `\\` and `\'`.

`use utf8;` makes Perl read the rest of the file as UTF-8 characters rather than bytes.
Malformed UTF-8 is then refused.

Inside the heredoc body:

- `$thoughts`, `$eyes`, `$tongue`, and any variable from the assignments above, interpolate.
  The `${name}` form works as well.
- The escapes are those of a Perl double-quoted string, listed below.
- An unknown `$name` is refused rather than interpolated.
  So is a `$` that no name follows: Perl reads `$?` or `$/` as a special variable,
  and skips spaces to find a name, so `$ /` means `$/` too.
- An unbraced `$name` followed by `[`, `{`, `->[`, `->{`, `::`, or `'` and a letter is refused.
  Perl reads those as an element or a package variable.
- `@` is refused before a letter, a digit, `_`, `:`, `'`, `{`, `$`, `+` or `-`,
  where Perl interpolates an array such as `@+`; before anything else it stays literal.

| Escape | Stands for |
| --- | --- |
| `\t`, `\n`, `\r`, `\f`, `\b`, `\a`, `\e` | that control character |
| `\cX` | the control character of `X`, as `\c[` for ESC |
| `\xHH`, `\x{HHHH}`, `\N{U+HHHH}` | the character of that hexadecimal code |
| `\101`, `\o{101}` | the character of that octal code, up to three digits without braces |
| `\` and any other character | that character, so `\$`, `\@`, `\_`, and a newline after `\` |

`\u`, `\l`, `\U`, `\L`, `\Q`, `\E` and `\F` are refused, since they change the case of what follows.
So is `\N{name}`, and a code of 0, of a surrogate, or past U+10FFFF.

Perl prints a cow with a character above U+00FF as UTF-8, and so does this implementation.
Perl then encodes each raw byte above 0x7F once more, so such a byte in that cow is refused.
Without that character, Perl prints U+0080 to U+00FF as a lone byte, so that character is refused.
It comes from an escape such as `\xA0`, or from the source under `use utf8;`.
