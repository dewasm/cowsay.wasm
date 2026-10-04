#!/usr/bin/env bash

# Every version the documents name matches its source:
# cowsay from the reference's --version, cowsay.wasm from cowsay.c, Unicode from the vendored UCD.
# A document may name a version any number of times; each one is checked.

set -u
cd "$(dirname "$0")/.." || exit 1

DOCS=(README.md test/README.md)
REF=test/submodules/cowsay-org-cowsay/bin/cowsay
if [ ! -f "$REF" ]; then
  echo "test/submodules/cowsay-org-cowsay is empty: run git submodule update --init" >&2
  exit 1
fi

cowsay_version=$(perl "$REF" --version | sed -n 's/.* version \([0-9.]*\) calling .*/\1/p')
wasm_version=$(sed -n 's/^#define COWSAY_WASM_VERSION "\(.*\)"/\1/p' cowsay.c)
unicode_version=$(sed -n '1s/^# EastAsianWidth-\(.*\)\.txt$/\1/p' ucd/EastAsianWidth.txt)
for source in "cowsay:$cowsay_version" "cowsay.wasm:$wasm_version" "Unicode:$unicode_version"; do
  if [ -z "${source#*:}" ]; then
    echo "check-docs: no ${source%%:*} version found in its source" >&2
    exit 1
  fi
done

fail=0
mentions=0
check() { # <name> <want> <regex>
  local name=$1 want=$2 regex=$3 hit
  while IFS= read -r hit; do
    mentions=$((mentions + 1))
    # A hit reads file:line:match, and the match ends in the version.
    if [ "${hit##* }" != "$want" ]; then
      echo "FAIL: ${hit%:*}: names ${hit##*:}, but $name is $want"
      fail=$((fail + 1))
    fi
  done < <(grep -HnoE "$regex" "${DOCS[@]}")
}
VERSION='[0-9]+(\.[0-9]+)+'
check cowsay "$cowsay_version" "cowsay (version )?$VERSION"
check cowsay.wasm "$wasm_version" "cowsay\.wasm $VERSION"
check Unicode "$unicode_version" "Unicode $VERSION"

if [ "$fail" -gt 0 ]; then
  echo "docs: $fail of $mentions versions wrong in ${DOCS[*]}"
  exit 1
fi
printf 'docs: %d versions in %s match cowsay %s, cowsay.wasm %s and Unicode %s\n' \
  "$mentions" "${DOCS[*]}" "$cowsay_version" "$wasm_version" "$unicode_version"
