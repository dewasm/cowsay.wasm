#!/usr/bin/env bash

# Differential test: our cowsay against the vendored reference, cowsay 3.03 under the host perl.
# Every case compares stdout, stderr and the exit code.
# Behaviors changed on purpose are pinned by snapshots under test/fixed/, via the fixed() helper.
# test/README.md lists them under "Deliberate fixes".
#
# Every case runs our binary twice: with COWPATH pointing at cows/, then without it.
# That covers the real-filesystem lookup and the embedded cows.
# The reference always runs with COWPATH.
# Cases marked "pathonly" skip the embedded run: -l prints the cowfile directory in its header.
#
# COWSAY_TEST_MODE=wasm runs cowsay.wasm under wasmtime instead of cowsay-native.

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
COWS=$ROOT/cows
REF=$ROOT/test/reference/cowsay
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
FAILLOG=$TMP/failures.log
: >"$FAILLOG"

# Progress display: one line per section, one dot per check, and the failures at the end.
# Colors are on when stdout is a terminal.
# NO_COLOR turns them off; CLICOLOR_FORCE keeps them when the output is redirected.
if { [ -t 1 ] || [ -n "${CLICOLOR_FORCE-}" ]; } && [ -z "${NO_COLOR-}" ]; then
  C_OK=$'\033[32m'
  C_BAD=$'\033[31m'
  C_NAME=$'\033[1m'
  C_DIM=$'\033[2m'
  C_OFF=$'\033[0m'
else
  C_OK='' C_BAD='' C_NAME='' C_DIM='' C_OFF=''
fi

DOTS_PER_LINE=64
LABEL_WIDTH=10
section_name=''
section_pass=0
section_fail=0
section_dots=0

section() { # <name>
  section_end
  section_name=$1
  section_pass=0
  section_fail=0
  section_dots=0
  # The dot color stays on for the whole section, so a run of checks costs one escape, not one each.
  printf '%s%-*s%s %s' "$C_NAME" "$LABEL_WIDTH" "$section_name" "$C_OFF" "$C_OK"
}

section_end() {
  [ -n "$section_name" ] || return 0
  printf '%s' "$C_OFF"
  if [ "$section_fail" -eq 0 ]; then
    printf ' %s%d ok%s\n' "$C_DIM" "$section_pass" "$C_OFF"
  else
    printf ' %s%d failed%s, %d ok\n' "$C_BAD" "$section_fail" "$C_OFF" "$section_pass"
  fi
  section_name=''
}

tick() { # <ok|failed>
  if [ "$1" = ok ]; then
    pass=$((pass + 1))
    section_pass=$((section_pass + 1))
    printf '.'
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
  COWPATH=$COWS perl "$script" "$@" <"$in" >"$TMP/ref.out" 2>"$TMP/ref.err"
  echo $? >"$TMP/ref.code"
}

run_ours() { # <think> <path|embedded> <stdin-file> [args...]
  local think=$1 cpmode=$2 in=$3
  shift 3
  if [ "$MODE" = wasm ]; then
    local mod=$TMP/cowsay
    [ "$think" = 1 ] && mod=$TMP/cowthink.wasm
    if [ "$cpmode" = path ]; then
      wasmtime run --dir "$COWS::$COWS" --env COWPATH="$COWS" "$mod" "$@" \
        <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    else
      wasmtime run "$mod" "$@" <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    fi
  else
    local bin=$TMP/cowsay
    [ "$think" = 1 ] && bin=$TMP/cowthink-native
    if [ "$cpmode" = path ]; then
      COWPATH=$COWS "$bin" "$@" <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
    else
      env -u COWPATH "$bin" "$@" <"$in" >"$TMP/got.out" 2>"$TMP/got.err"
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

# A deliberate fix diverges from the reference on purpose, so snapshots pin it instead:
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

section basics
t one-word '' hi
t sentence '' Hello from dewasm!
t wrap-long '' The quick brown fox jumps over the lazy dog and afterwards does it again for good measure.
t huge-word '' xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
t huge-word-tail '' aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa end
t width-10 '' -W 10 abcdefghijklmnopqrstuv end
t width-2 '' -W 2 hello there
t width-attached '' -W10 abcdefghijklmnop
# Deliberate fix: any width below 2 behaves as 2 (the reference makes the message "3").
fixed width-0 0 '' -W 0 tiny width
fixed width-1 0 '' -W 1 tiny width
fixed width-junk 0 '' -W abc tiny width
fixed width-negative 0 '' -W -5 tiny width

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
# Deliberate fix: any remaining argument selects the argument message.
# The reference treats a first argument of "0" or "" as false and reads stdin.
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
t cow-three-eyes-e '' -e ab -f three-eyes moo
t cow-udder-e '' -e ab -f udder moo
t cow-missing '' -f nosuch moo
# Deliberate fix: a missing path with a slash is an error (the reference exits 0 with no cow).
fixed cow-slash-missing 2 '' -f /no/such/file.cow moo
# Needs the real filesystem: without a preopen the wasm build cannot open the absolute path.
t_pathonly cow-slash-path '' -f "$COWS/default.cow" moo

section usage
# Deliberate fix: usage exits with EX_USAGE (64) instead of the reference's 255.
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

# The reference is byte-based here, so these cases check our own specification instead.
section utf8
utf8() { # <name> <stdin-string> [args...]
  local name=$1 stdin=$2
  shift 2
  printf '%s' "$stdin" >"$TMP/in"
  run_ours 0 embedded "$TMP/in" "$@"
  if ! perl -e '
    local $/; my $s = <STDIN>;
    exit(utf8::decode($s) ? 0 : 1);
  ' <"$TMP/got.out"; then
    tick failed
    echo "FAIL: utf8-$name produced invalid UTF-8" >>"$FAILLOG"
  elif ! cmp -s "$TMP/got.out" "test/utf8/$name.expected"; then
    tick failed
    {
      echo "FAIL: utf8-$name differs from test/utf8/$name.expected"
      diff -u "test/utf8/$name.expected" "$TMP/got.out" | head -20 | sed 's/^/  /'
    } >>"$FAILLOG"
  else
    tick ok
  fi
}

utf8 latin '' 'héllo wörld, ça va très bien aujourdʼhui'
utf8 cjk '' 'こんにちは世界'
utf8 wrap-boundary '' -W 10 'ééééééééééééééééééééééééé'
utf8 eyes '' -e 'øø' moo
utf8 mixed $'Ünïcödé line one\nsecond line ここ\n'

section_end
if [ "$fail" -gt 0 ]; then
  printf '\n'
  cat "$FAILLOG"
fi
# The trailing blank line keeps the two modes of `make check` apart.
if [ "$fail" -eq 0 ]; then
  printf '\n%s%s: %d ok%s in %ds\n\n' "$C_OK" "$MODE" "$pass" "$C_OFF" "$SECONDS"
else
  printf '\n%s%s: %d failed%s, %d ok, in %ds\n\n' \
    "$C_BAD" "$MODE" "$fail" "$C_OFF" "$pass" "$SECONDS"
fi
[ "$fail" -eq 0 ]
