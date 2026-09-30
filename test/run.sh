#!/usr/bin/env bash

# Differential test: our cowsay against the vendored reference, cowsay 3.03 under the host perl.
# Every case compares stdout, stderr and the exit code.
# Behaviors changed on purpose are pinned by snapshots under test/fixed/, via the fixed() helper.
# test/README.md lists them under "Intended fixes".
#
# Every case runs our binary twice: with COWPATH pointing at cows/, then without it.
# That covers the real-filesystem lookup and the embedded cows.
# The reference always runs with COWPATH.
# Cases marked "pathonly" skip the embedded run: -l prints the cowfile directory in its header.
#
# COWSAY_TEST_MODE=wasm runs cowsay.wasm under wasmtime instead of cowsay-native.
#
# `run.sh --third-party-cows` runs every cowfile of the submodules under test/submodules instead.

set -u
cd "$(dirname "$0")/.." || exit 1

# Column widths depend on the environment, and a wasm run sees none of it.
# A native run is given none either, and the width cases hand in what they need one at a time.
unset LANG LC_ALL LC_CTYPE COWSAY_AMBIGUOUS_WIDTH
ROOT=$PWD
COWS=$ROOT/cows
REF=$ROOT/test/reference/cowsay
# Each submodule under test/submodules, with the directory of its cowfiles.
SUBMODULES=(
  paulkaefer-cowsay-files/cows
  cowsay-org-cowsay/share/cowsay/cows
  phmajerus-cowfiles/cows
  mstill3-cowsay-files/cows
)
for entry in "${SUBMODULES[@]}"; do
  if [ ! -d "$ROOT/test/submodules/$entry" ]; then
    echo "test/submodules/${entry%%/*} is empty: run git submodule update --init" >&2
    exit 1
  fi
done

submodule_cows() { # <submodule>
  local entry
  for entry in "${SUBMODULES[@]}"; do
    [ "${entry%%/*}" = "$1" ] && echo "$ROOT/test/submodules/$entry"
  done
}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The binary under test runs from a copy named "cowsay" (or "cowthink").
# The reference takes its program name and think mode from $0, and ours from argv[0].
MODE=${COWSAY_TEST_MODE:-native}
cp "$REF" "$TMP/cowthink"
if [ "$MODE" = wasm ]; then
  cp cowsay.wasm "$TMP/cowsay"
  cp cowsay.wasm "$TMP/cowthink.wasm"
else
  cp cowsay-native "$TMP/cowsay"
  cp cowsay-native "$TMP/cowthink-native"
fi

pass=0
fail=0
refused=0
FAILLOG=$TMP/failures.log
: >"$FAILLOG"

# Progress display: one line per section, one dot per check, and the failures at the end.
# A third-party cowfile outside the grammar shows as an r: refused, which is not a failure.
# Colors are on when stdout is a terminal.
# NO_COLOR turns them off; CLICOLOR_FORCE keeps them when the output is redirected.
if { [ -t 1 ] || [ -n "${CLICOLOR_FORCE-}" ]; } && [ -z "${NO_COLOR-}" ]; then
  C_OK=$'\033[32m'
  C_BAD=$'\033[31m'
  C_REFUSED=$'\033[33m'
  C_NAME=$'\033[1m'
  C_DIM=$'\033[2m'
  C_OFF=$'\033[0m'
else
  C_OK='' C_BAD='' C_REFUSED='' C_NAME='' C_DIM='' C_OFF=''
fi

DOTS_PER_LINE=64
LABEL_WIDTH=10
section_name=''
section_pass=0
section_fail=0
section_refused=0
section_dots=0

section() { # <name>
  section_end
  section_name=$1
  section_pass=0
  section_fail=0
  section_refused=0
  section_dots=0
  # The dot color stays on for the whole section, so a run of checks costs one escape, not one each.
  printf '%s%-*s%s %s' "$C_NAME" "$LABEL_WIDTH" "$section_name" "$C_OFF" "$C_OK"
}

section_end() {
  [ -n "$section_name" ] || return 0
  printf '%s' "$C_OFF"
  if [ "$section_fail" -gt 0 ]; then
    printf ' %s%d failed%s,' "$C_BAD" "$section_fail" "$C_OFF"
  fi
  if [ "$section_refused" -gt 0 ]; then
    printf ' %s%d refused%s,' "$C_REFUSED" "$section_refused" "$C_OFF"
  fi
  printf ' %s%d ok%s\n' "$C_DIM" "$section_pass" "$C_OFF"
  section_name=''
}

tick() { # <ok|refused|failed>
  if [ "$1" = ok ]; then
    pass=$((pass + 1))
    section_pass=$((section_pass + 1))
    printf '.'
  elif [ "$1" = refused ]; then
    refused=$((refused + 1))
    section_refused=$((section_refused + 1))
    printf '%sr%s' "$C_REFUSED" "$C_OK"
  else
    fail=$((fail + 1))
    section_fail=$((section_fail + 1))
    printf '%sF%s' "$C_BAD" "$C_OK"
  fi
  section_dots=$((section_dots + 1))
  [ $((section_dots % DOTS_PER_LINE)) -eq 0 ] && printf '\n%*s ' "$LABEL_WIDTH" ''
  return 0
}

if [ "$MODE" = wasm ]; then
  size=$(wc -c <cowsay.wasm | awk '{printf "%.1f kB", $1 / 1000}')
  under="cowsay.wasm, $size, under $(wasmtime --version | cut -d' ' -f1-2)"
else
  under="cowsay-native"
fi
printf '%s%s mode%s: %s\n' "$C_NAME" "$MODE" "$C_OFF" "$under"
printf 'reference: cowsay 3.03 under perl %s\n\n' "$(perl -e 'print $^V')"

run_ref() { # <think> <stdin-file> [args...]
  local think=$1 in=$2
  shift 2
  local script=$REF
  [ "$think" = 1 ] && script=$TMP/cowthink
  COWPATH=$COWS perl "$script" "$@" <"$in" >"$TMP/ref.out" 2>"$TMP/ref.raw.err"
  echo $? >"$TMP/ref.code"
  # Intended fix: Perl warns when it prints a cow as UTF-8, and we print no such warning.
  grep -v '^Wide character in print at .* line [0-9]*\.$' "$TMP/ref.raw.err" >"$TMP/ref.err"
}

# WIDTH_ENV names the variables a width case wants handed to the binary, as NAME=VALUE words.
WIDTH_ENV=

run_ours() { # <think> <path|embedded> <stdin-file> [args...]
  local think=$1 cpmode=$2 in=$3
  shift 3
  local wasm_env=() native_env=() kv
  for kv in $WIDTH_ENV; do
    wasm_env+=(--env "$kv")
    native_env+=("$kv")
  done
  if [ "$MODE" = wasm ]; then
    local mod=$TMP/cowsay
    [ "$think" = 1 ] && mod=$TMP/cowthink.wasm
    if [ "$cpmode" = path ]; then
      wasmtime run --dir "$COWS::$COWS" --env COWPATH="$COWS" \
        ${wasm_env+"${wasm_env[@]}"} "$mod" "$@" <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    else
      wasmtime run ${wasm_env+"${wasm_env[@]}"} "$mod" "$@" <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    fi
  else
    local bin=$TMP/cowsay
    [ "$think" = 1 ] && bin=$TMP/cowthink-native
    if [ "$cpmode" = path ]; then
      env COWPATH="$COWS" ${native_env+"${native_env[@]}"} "$bin" "$@" \
        <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    else
      env -u COWPATH ${native_env+"${native_env[@]}"} "$bin" "$@" \
        <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    fi
  fi
  echo $? >"$TMP/got.code"
}

report() { # <name> <cpmode> [args...]
  local name=$1 cpmode=$2
  shift 2
  if cmp -s "$TMP/ref.out" "$TMP/got.out" &&
     cmp -s "$TMP/ref.err" "$TMP/got.err" &&
     cmp -s "$TMP/ref.code" "$TMP/got.code"; then
    tick ok
  else
    tick failed
    {
      echo "FAIL: $name [$cpmode] args:" "$@"
      echo "  exit: ref=$(cat "$TMP/ref.code") got=$(cat "$TMP/got.code")"
      diff -u "$TMP/ref.out" "$TMP/got.out" | head -30 | sed 's/^/  out /'
      diff -u "$TMP/ref.err" "$TMP/got.err" | head -10 | sed 's/^/  err /'
    } >>"$FAILLOG"
  fi
}

t() { # <name> <stdin-string> [args...]
  local name=$1 stdin=$2
  shift 2
  printf '%s' "$stdin" >"$TMP/in"
  run_ref 0 "$TMP/in" "$@"
  run_ours 0 path "$TMP/in" "$@"
  report "$name" path "$@"
  run_ours 0 embedded "$TMP/in" "$@"
  report "$name" embedded "$@"
}

t_think() { # <name> <stdin-string> [args...]
  local name=$1 stdin=$2
  shift 2
  printf '%s' "$stdin" >"$TMP/in"
  run_ref 1 "$TMP/in" "$@"
  run_ours 1 path "$TMP/in" "$@"
  report "$name" path "$@"
  run_ours 1 embedded "$TMP/in" "$@"
  report "$name" embedded "$@"
}

t_pathonly() { # <name> <stdin-string> [args...]
  local name=$1 stdin=$2
  shift 2
  printf '%s' "$stdin" >"$TMP/in"
  run_ref 0 "$TMP/in" "$@"
  run_ours 0 path "$TMP/in" "$@"
  report "$name" path "$@"
}

# An intended fix diverges from the reference on purpose, so snapshots pin it instead:
# test/fixed/<name>.out, and <name>.err when stderr is expected.
fixed() { # <name> <expected-exit> <stdin-string> [args...]
  local name=$1 code=$2 stdin=$3
  shift 3
  printf '%s' "$stdin" >"$TMP/in"
  local cpmode
  for cpmode in path embedded; do
    run_ours 0 "$cpmode" "$TMP/in" "$@"
    local ok=1
    cmp -s "$TMP/got.out" "test/fixed/$name.out" || ok=0
    if [ -f "test/fixed/$name.err" ]; then
      cmp -s "$TMP/got.err" "test/fixed/$name.err" || ok=0
    elif [ -s "$TMP/got.err" ]; then
      ok=0
    fi
    [ "$(cat "$TMP/got.code")" = "$code" ] || ok=0
    if [ "$ok" = 1 ]; then
      tick ok
    else
      tick failed
      {
        echo "FAIL: fixed-$name [$cpmode] args:" "$@"
        echo "  exit: want=$code got=$(cat "$TMP/got.code")"
        diff -u "test/fixed/$name.out" "$TMP/got.out" | head -20 | sed 's/^/  out /'
        [ -f "test/fixed/$name.err" ] &&
          diff -u "test/fixed/$name.err" "$TMP/got.err" | head -10 | sed 's/^/  err /'
      } >>"$FAILLOG"
    fi
  done
}

# A cowfile refused with an error naming the line, never rendered.
expect_refused() { # <label> <dir> <name> <line> <message>
  printf '%s\n' "cowsay: $2/$3.cow:$4: $5" >"$TMP/want.err"
  COWS=$2 run_ours 0 path /dev/null -f "$3" moo
  if [ "$(cat "$TMP/got.code")" = 1 ] && cmp -s "$TMP/want.err" "$TMP/got.err"; then
    tick ok
  else
    tick failed
    {
      echo "FAIL: $1: exit $(cat "$TMP/got.code")"
      diff -u "$TMP/want.err" "$TMP/got.err" | head -10 | sed 's/^/  err /'
    } >>"$FAILLOG"
  fi
}

finish() {
  section_end
  if [ "$fail" -gt 0 ]; then
    printf '\n'
    cat "$FAILLOG"
  fi
  # The trailing blank line keeps the two modes of `make check` apart.
  if [ "$fail" -eq 0 ]; then
    printf '\n%s%s:%s' "$C_OK" "$MODE" "$C_OFF"
  else
    printf '\n%s%s: %d failed%s,' "$C_BAD" "$MODE" "$fail" "$C_OFF"
  fi
  if [ "$refused" -gt 0 ]; then
    printf ' %s%d refused%s,' "$C_REFUSED" "$refused" "$C_OFF"
  fi
  printf ' %d ok in %ds\n\n' "$pass" "$SECONDS"
  [ "$fail" -eq 0 ]
}

# Each cowfile of a submodule matches the reference, or is refused with an error naming the line.
# A cowfile is Perl code that the reference runs with `do`,
# so the reference only sees what our parser accepted.
all_of_submodule() { # <submodule>
  local dir cow name
  dir=$(submodule_cows "$1")
  : >"$TMP/reasons"
  section "$1"
  for cow in "$dir"/*.cow; do
    name=$(basename "$cow" .cow)
    COWS=$dir run_ours 0 path /dev/null -f "$name" moo
    if [ "$(cat "$TMP/got.code")" = 1 ] && [[ "$(cat "$TMP/got.err")" == "cowsay: $cow:"* ]]; then
      sed 's/^.*:[0-9]*: //' "$TMP/got.err" >>"$TMP/reasons"
      tick refused
      continue
    fi
    COWS=$dir run_ref 0 /dev/null -f "$name" moo
    report "$1-$name" path -f "$name" moo
  done
  section_end
  sort "$TMP/reasons" | uniq -c | sort -rn |
    awk -v w="$LABEL_WIDTH" '{ n = $1; sub(/^ *[0-9]+ /, ""); printf "%*s %4d %s\n", w, "", n, $0 }'
}

if [ "${1-}" = --third-party-cows ]; then
  for entry in "${SUBMODULES[@]}"; do
    entry=${entry%%/*}
    [ "${#entry}" -gt "$LABEL_WIDTH" ] && LABEL_WIDTH=${#entry}
  done
  for entry in "${SUBMODULES[@]}"; do
    all_of_submodule "${entry%%/*}"
  done
  finish
  exit
fi

section basics
t one-word '' hi
t sentence '' Hello from dewasm!
t wrap-long '' The quick brown fox jumps over the lazy dog and afterwards does it again for good measure.
t huge-word '' xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
t huge-word-tail '' aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa end
t width-10 '' -W 10 abcdefghijklmnopqrstuv end
t width-2 '' -W 2 hello there
t width-attached '' -W10 abcdefghijklmnop
# Intended fix: any width below 2 behaves as 2 (the reference makes the message "3").
fixed width-0 0 '' -W 0 tiny width
fixed width-1 0 '' -W 1 tiny width
fixed width-junk 0 '' -W abc tiny width
fixed width-negative 0 '' -W -5 tiny width
# Intended fix: an SGR sequence costs no columns, and a colour still open at a wrap is
# reopened on the next line (the reference counts the escape bytes as text).
fixed escape-width 0 '' $'\033[31mred\033[0m and plain'
fixed escape-wrap 0 '' -W 24 $'\033[1;31mthis red sentence is long enough to wrap twice over\033[0m'
fixed escape-only 0 '' $'\033[31m\033[0m'

section stdin
t stdin-multiline $'line one\nline two\nline three\n'
t stdin-paragraphs $'first paragraph\n\nsecond paragraph\n'
t stdin-indent $'first line\n  indented continuation\ntail\n'
t stdin-spaces $'a  b   c\nd    e\n'
t stdin-tabs $'a\tb\tc\n'
t stdin-trailing-space $'word \nanother  \n'
t stdin-no-final-newline $'no newline at end'
t stdin-empty ''
t stdin-only-newline $'\n'
t stdin-space-line $' \n'
t stdin-blank-leading $'\n\nafter blanks\n'
t n-tabs $'a\tb\n12345678\tx\n' -n
t n-multiline $'keep   these\n  lines as-is\n' -n
t n-empty '' -n

section arguments
# Intended fix: any remaining argument selects the argument message.
# The reference tests `unless ($ARGV[0])`, where Perl takes "0" and "" as false, and reads stdin.
fixed arg-zero 0 $'not this\n' 0 is a message
fixed arg-empty-first 0 $'not this\n' '' still a message
t arg-space ' ' ' '
t double-dash '' -- -b not an option
t rewritten-terminator '' -b- after rewritten terminator
t cluster '' -bd moo
t unknown-option '' -Z moo
t unknown-cluster '' -Zb moo
t opt-arg-missing '' -e

section faces
for f in b d g p s t w y; do
  t "flag-$f" '' -$f moo
done
t flag-order '' -y -b moo
t flag-overrides-e '' -d -e '@@' moo
t eyes-short '' -e a moo
t eyes-long '' -e abcd moo
t tongue '' -T uu moo
t tongue-short '' -T u moo
t eyes-empty '' -e '' moo

section cowfiles
for cow in cows/*.cow; do
  name=$(basename "$cow" .cow)
  t "cow-$name" '' -f "$name" moo
done
t cow-suffix '' -f default.cow moo
t cow-small-empty-eyes '' -e '' -f small moo
# Intended fix: `-e 0` keeps its eye.
# Perl takes "0" as false, so the reference fills in the default.
fixed cow-small-zero-eyes 0 '' -e 0 -f small moo
t cow-three-eyes-e '' -e ab -f three-eyes moo
t cow-udder-e '' -e ab -f udder moo
t cow-clawd-eyes '' -e '><' -f clawd moo
t cow-clawd-one-eye '' -e X -f clawd moo
t cow-clawd-no-eyes '' -e '' -f clawd moo
t cow-clawd-default-eyes '' -e oo -f clawd moo
t cow-clawd-dead '' -d -f clawd moo
t cow-missing '' -f nosuch moo
# Intended fix: a missing path with a slash is an error (the reference exits 0 with no cow).
fixed cow-slash-missing 2 '' -f /no/such/file.cow moo
# Needs the real filesystem: without a preopen the wasm build cannot open the absolute path.
t_pathonly cow-slash-path '' -f "$COWS/default.cow" moo

section heredoc
# Each case is a cowfile of one body line, found through a COWPATH of its own.
BODIES=$TMP/bodies
mkdir "$BODIES"

# The single quotes here and in the cases keep the Perl text from the shell.
# shellcheck disable=SC2016
write_body() { # <name> <body-line>
  printf '$the_cow = <<EOC;\n%s\nEOC\n' "$2" >"$BODIES/$1.cow"
}

body_case() { # <name> <body-line>
  write_body "$1" "$2"
  COWS=$BODIES t_pathonly "body-$1" '' -f "$1" moo
}

# Perl interpolates these, often to a value of the process, so the grammar refuses them.
refused() { # <name> <body-line> <message>
  write_body "$1" "$2"
  expect_refused "refused-$1" "$BODIES" "$1" 2 "$3"
}

# shellcheck disable=SC2016
{
  body_case at-space 'a @ b'
  body_case at-at 'a@@ b'
  body_case at-paren 'a@) b'
  body_case arrow-word 'a$eyes->x b'
  body_case single-colon 'a$eyes:x b'
  body_case braced-subscript 'a${eyes}[0] b'
  body_case escaped '\$? \@+ \$eyes[0]'
  refused dollar-punct 'a$? b' 'unescaped $ in cowfile'
  refused dollar-space 'a$ /b' 'unescaped $ in cowfile'
  refused dollar-eol 'a$' 'unescaped $ in cowfile'
  refused dollar-digit 'a$1 b' 'unescaped $ in cowfile'
  refused element 'a$eyes[0] b' 'unsupported subscript in cowfile'
  refused hash-element 'a$eyes{x} b' 'unsupported subscript in cowfile'
  refused arrow-element 'a$eyes->[0] b' 'unsupported subscript in cowfile'
  refused package 'a$eyes::x b' 'unsupported subscript in cowfile'
  refused old-package "a\$eyes'x b" 'unsupported subscript in cowfile'
  refused at-digit 'a@9 b' 'unescaped @ in cowfile'
  refused at-underscore 'a@_ b' 'unescaped @ in cowfile'
  refused at-colon 'a@: b' 'unescaped @ in cowfile'
  refused at-quote "a@'b" 'unescaped @ in cowfile'
  refused at-brace 'a@{b}' 'unescaped @ in cowfile'
  refused at-dollar 'a@$b' 'unescaped @ in cowfile'
  refused at-plus 'a@+ b' 'unescaped @ in cowfile'
  refused at-minus 'a@- b' 'unescaped @ in cowfile'
}

# The escapes of a Perl double-quoted string.
# A character above U+00FF makes Perl print the cow as UTF-8, and \x80 to \xFF with it.
# shellcheck disable=SC2016,SC1003
{
  body_case escape-controls 'a\tb\ac\bd\fe\rf\eg'
  body_case escape-control-letters '\cA\cz\c?\c['
  body_case escape-hex '\x41\x4g'
  body_case escape-hex-braced '\x{41}\x{263A}'
  body_case escape-unicode '\N{U+263A}'
  body_case escape-octal '\101\7\0101\o{101}'
  body_case escape-digits '\8\9'
  body_case escape-others '\_\ \K\"\/\#'
  body_case escape-byte '\é'
  body_case escape-latin1-wide '\xA0\x{2580}'
  body_case escape-newline 'a\'
  refused escape-case 'a\ub' 'unsupported escape in cowfile'
  refused escape-name '\N{LATIN SMALL LETTER A}' 'unsupported escape in cowfile'
  refused escape-nul '\0' 'unsupported escape in cowfile'
  refused escape-nul-braced '\x{0}' 'unsupported escape in cowfile'
  refused escape-surrogate '\x{D800}' 'unsupported escape in cowfile'
  refused escape-octal-unbraced '\o1' 'unsupported escape in cowfile'
  refused escape-hex-junk '\x{zz}' 'unsupported escape in cowfile'
  # Perl would print a lone byte for \xA0, or encode the raw é once more.
  refused escape-latin1-alone '\xA0' 'unsupported character U+0080-U+00FF'
  refused escape-byte-beside-wide 'é\x{263A}' 'unsupported byte beside a wide character'
}

section preamble
# Each case is a whole cowfile, one argument per line.
write_cow() { # <name> <line>...
  local name=$1
  shift
  printf '%s\n' "$@" >"$BODIES/$name.cow"
}

cow_case() { # <name> <line>...
  write_cow "$@"
  COWS=$BODIES t_pathonly "preamble-$1" '' -f "$1" moo
}

cow_refused() { # <name> <line-number> <message> <line>...
  local name=$1 at=$2 message=$3
  shift 3
  write_cow "$name" "$@"
  expect_refused "preamble-$name" "$BODIES" "$name" "$at" "$message"
}

# shellcheck disable=SC2016
{
  cow_case assign '$x = "\e[49m  ";          #reset color' '$the_cow = <<EOC' '[$x]' 'EOC'
  cow_case assign-thoughts '$t = "$thoughts ";' '$the_cow = <<EOC;' '$t$t' 'EOC'
  cow_case assign-variables '$a = "<";' '$b = "$a${a}$eyes$tongue";' '$the_cow = <<EOC;' '$b' 'EOC'
  cow_case assign-comment-text '$x = "a # b; c";' '$the_cow = <<EOC;' '$x' 'EOC'
  cow_case assign-escapes '$x = "\"\\\$\@";' '$the_cow = <<EOC;' '$x' 'EOC'
  cow_case assign-again '$x = "a";' '$x = "b";' '$the_cow = <<EOC;' '$x' 'EOC'
  cow_case assign-eyes '$eyes = "^^"; # always' '$the_cow = <<EOC;' '($eyes)' 'EOC'
  cow_case append '$x = "a";' '$x .= " $tongue!";' '$the_cow = <<EOC;' '$x' 'EOC'
  cow_case append-tongue '$tongue .= "!";' '$the_cow = <<EOC;' '$tongue' 'EOC'
  cow_case idiom-comment '$extra = chop($eyes);  # the third eye' \
    '$the_cow = <<EOC;' '$eyes$extra' 'EOC'
  cow_case heredoc-comment '$the_cow = <<EOC; # the cow' '$thoughts' 'EOC'
  cow_case heredoc-comment-no-semicolon '$the_cow = <<EOC # the cow' '$thoughts' 'EOC'
  cow_case assign-escapes-wide '$x = "\t\x41\xA0";' '$y = "\x{263A}";' \
    '$the_cow = <<EOC;' '$x$y' 'EOC'
  cow_refused escape 1 'unsupported escape in cowfile' '$x = "\Ua";' '$the_cow = <<EOC;' 'EOC'
  cow_refused latin1-unused-wide 1 'unsupported character U+0080-U+00FF' \
    '$x = "\xA0";' '$y = "\x{263A}";' '$the_cow = <<EOC;' '$x' 'EOC'
  cow_refused code 1 'unescaped @ in cowfile' '$x = "@{[ 1 ]}";' '$the_cow = <<EOC;' 'EOC'
  cow_refused code-in-idiom 1 'unescaped @ in cowfile' \
    '$eyes .= "@{[ 1 ]}";' '$the_cow = <<EOC;' 'EOC'
  cow_refused code-in-condition 1 'unescaped @ in cowfile' \
    '$eyes = "" if ($eyes eq "@{[ 1 ]}");' '$the_cow = <<EOC;' 'EOC'
  cow_refused scalar-code 1 'unsupported ${...} form' '$x = "${\ 1}";' '$the_cow = <<EOC;' 'EOC'
  cow_refused unknown 1 'unknown variable in cowfile' '$x = "$y";' '$the_cow = <<EOC;' 'EOC'
  cow_refused two-lines 1 'unsupported cowfile construct' '$x = "a' 'b";' '$the_cow = <<EOC;' 'EOC'
  cow_refused no-semicolon 1 'unsupported cowfile construct' '$x = "a"' '$the_cow = <<EOC;' 'EOC'
  cow_refused append-unknown 1 'unsupported cowfile construct' \
    '$y .= "a";' '$the_cow = <<EOC;' 'EOC'
  # Single quotes: only \\ and \' are escapes, and nothing interpolates.
  cow_case single-quoted "\$x = 'a\\\\b\\'c\\n\$d@e';" '$the_cow = <<EOC;' '$x' 'EOC'
  cow_case unless-ne "\$eyes = '..' unless (\$eyes ne 'oo');" '$the_cow = <<EOC;' '$eyes' 'EOC'
  cow_case if-ne '$eyes = ".." if ($eyes ne "oo");' '$the_cow = <<EOC;' '$eyes' 'EOC'
  # use utf8: the source is UTF-8 characters, so a character above U+00FF makes the cow UTF-8.
  cow_case use-utf8 'use utf8; # characters' '$x = "é";' '$the_cow = <<EOC;' '$x─' 'EOC'
  cow_refused utf8-latin1 3 'unsupported character U+0080-U+00FF' \
    'use utf8;' '$the_cow = <<EOC;' 'é' 'EOC'
  cow_refused utf8-malformed 3 'malformed UTF-8 in cowfile' \
    'use utf8;' '$the_cow = <<EOC;' $'\xff' 'EOC'
  # <<'EOC': no interpolation, and a backslash is text too.
  cow_case literal-heredoc "\$the_cow = <<'EOC';" 'a\\b\$x\n$thoughts @y' 'EOC'
  cow_case literal-heredoc-space "\$the_cow = << 'EOC';" 'x' 'EOC'
  cow_case quoted-heredoc-space '$the_cow = << "EOC";' '$thoughts' 'EOC'
  cow_refused bare-heredoc-space 1 'unsupported heredoc terminator' '$the_cow = << EOC;' 'x' 'EOC'
  # Perl drops a CR before an LF anywhere in the source, and keeps a lone CR.
  cow_case crlf $'# a comment\r' $'$x = "a"; # set\r' $'$the_cow = <<EOC;\r' $'$x $thoughts\\\r' \
    $'b\r' $'EOC\r'
  cow_case crlf-one-line '$the_cow = <<EOC;' $'a\r' 'EOC'
  cow_case crlf-terminator '$the_cow = <<EOC;' 'a' $'EOC\r'
  cow_case lone-cr '$the_cow = <<EOC;' $'a\rb' 'EOC'
}

section submodules
# Cowfiles of the submodules under test/submodules; `make check-third-party-cows` runs all of them.
submodule_case() { # <submodule> <name>
  COWS=$(submodule_cows "$1") t_pathonly "$1-$2" '' -f "$2" moo
}

submodule_refused() { # <submodule> <name> <line-number> <message>
  expect_refused "$1-$2" "$(submodule_cows "$1")" "$2" "$3" "$4"
}

# paulkaefer-cowsay-files: one cowfile for each shape the collection writes.
# Charc0al's converter; most omit the semicolon after <<EOC and put two spaces before the comment.
submodule_case paulkaefer-cowsay-files abu-apple
submodule_case paulkaefer-cowsay-files 47
submodule_case paulkaefer-cowsay-files cartman
# Many variables, some named with two letters.
submodule_case paulkaefer-cowsay-files baby_yoda
# $x inside a literal.
submodule_case paulkaefer-cowsay-files ignignokt
# No comment after the assignments.
submodule_case paulkaefer-cowsay-files err
# Drawn by hand.
submodule_case paulkaefer-cowsay-files kidcat
submodule_case paulkaefer-cowsay-files tortoise
submodule_case paulkaefer-cowsay-files USA
# Escapes in the heredoc: \_ and a \ before a space.
submodule_case paulkaefer-cowsay-files atat
# CRLF line ends.
submodule_case paulkaefer-cowsay-files chiyo-chichi
# Outside the grammar.
submodule_refused paulkaefer-cowsay-files cake 8 'unescaped @ in cowfile'
submodule_refused paulkaefer-cowsay-files golden-eagle 8 'unescaped $ in cowfile'

# cowsay-org-cowsay: the cowfiles that are new since 3.03 or changed; cows/ covers the rest.
submodule_case cowsay-org-cowsay actually
submodule_case cowsay-org-cowsay alpaca
submodule_case cowsay-org-cowsay cupcake
submodule_case cowsay-org-cowsay fox
submodule_case cowsay-org-cowsay kiss
submodule_case cowsay-org-cowsay llama
submodule_case cowsay-org-cowsay mech-and-cow
# Single quotes and `ne`.
submodule_case cowsay-org-cowsay sus

# phmajerus-cowfiles: color and Unicode, as \x1B, \x{...} and \xA0 beside them.
submodule_case phmajerus-cowfiles alexkidd
# CRLF line ends.
submodule_case phmajerus-cowfiles clippit

# mstill3-cowsay-files: drawn by hand, most under a comment header.
submodule_case mstill3-cowsay-files aardvark
submodule_case mstill3-cowsay-files bird-stork
# A \ at the end of a heredoc line.
submodule_case mstill3-cowsay-files chopper
# Outside the grammar.
submodule_refused mstill3-cowsay-files griffin 9 'unknown variable in cowfile'
submodule_refused mstill3-cowsay-files motivational-whale 19 \
  'unsupported text after heredoc terminator'
submodule_refused mstill3-cowsay-files snail 12 'unescaped @ in cowfile'

section usage
# Intended fix: usage exits with EX_USAGE (64) instead of the reference's 255.
# The usage text also drops a stray trailing space.
fixed usage-h 64 '' -h
fixed usage-n-args 64 '' -n moo
fixed usage-h-precedence 64 '' -h -l
t_pathonly list '' -l
t_pathonly list-ignores-W '' -l -W 10

section cowthink
t_think think-one '' moo
t_think think-multi '' this balloon has more than one line in it for sure
t_think think-stdin $'a\nb\n'

section fuzz
FUZZ=$TMP/fuzz
mkdir "$FUZZ"
perl test/gen-fuzz.pl "$FUZZ" >/dev/null
for dir in "$FUZZ"/*/; do
  args=()
  while IFS= read -r -d '' a; do args+=("$a"); done <"$dir/argv"
  cp "$dir/stdin" "$TMP/in"
  run_ref 0 "$TMP/in" ${args+"${args[@]}"}
  run_ours 0 path "$TMP/in" ${args+"${args[@]}"}
  report "fuzz-$(basename "$dir")" path ${args+"${args[@]}"}
  run_ours 0 embedded "$TMP/in" ${args+"${args[@]}"}
  report "fuzz-$(basename "$dir")" embedded ${args+"${args[@]}"}
done

# The reference measures bytes, so it cannot describe any of this;
# these cases check our own specification.
section width
width_case() { # <name> <stdin-string> [args...]
  local name=$1 stdin=$2
  shift 2
  printf '%s' "$stdin" >"$TMP/in"
  run_ours 0 embedded "$TMP/in" "$@"
  if ! perl -e '
    local $/; my $s = <STDIN>;
    exit(utf8::decode($s) ? 0 : 1);
  ' <"$TMP/got.out"; then
    tick failed
    echo "FAIL: width-$name produced invalid UTF-8" >>"$FAILLOG"
  elif ! cmp -s "$TMP/got.out" "test/width/$name.expected"; then
    tick failed
    {
      echo "FAIL: width-$name differs from test/width/$name.expected"
      diff -u "test/width/$name.expected" "$TMP/got.out" | head -20 | sed 's/^/  /'
    } >>"$FAILLOG"
  else
    tick ok
  fi
}

width_env() { # <NAME=VALUE...> -- <name> <stdin-string> [args...]
  local saved=$WIDTH_ENV
  WIDTH_ENV=
  while [ "$1" != -- ]; do
    WIDTH_ENV="$WIDTH_ENV $1"
    shift
  done
  shift
  width_case "$@"
  WIDTH_ENV=$saved
}

width_case latin '' 'héllo wörld, ça va très bien aujourdʼhui'
width_case cjk '' 'こんにちは世界'
width_case wrap-boundary '' -W 10 'ééééééééééééééééééééééééé'
width_case eyes '' -e 'øø' moo
width_case mixed $'Ünïcödé line one\nsecond line ここ\n'
# Widths are per grapheme cluster, so a combining mark, a flag and a ZWJ sequence each stay whole.
width_case combining '' -W 12 'éééééééééééééééé'
width_case cjk-wrap '' -W 12 '日本語のテキストを折り返す'
width_case emoji '' '👨‍👩‍👧 family, 🇯🇵 flag, 1️⃣ keycap'
width_case emoji-wrap '' -W 10 '🐄🐄🐄🐄🐄🐄🐄🐄'
# A tab under -n runs to the next multiple of eight columns, not of eight characters.
width_case tabs $'日本\tx\nab\tx\n' -n
# East Asian Ambiguous is one column by default, two under a CJK locale;
# the override settles it either way.
width_case ambiguous-default '' '§§§ ±±± °°°'
width_env LANG=ja_JP.UTF-8 -- ambiguous-locale '' '§§§ ±±± °°°'
width_env LANG=ja_JP.UTF-8 COWSAY_AMBIGUOUS_WIDTH=1 -- ambiguous-override '' '§§§ ±±± °°°'

finish
