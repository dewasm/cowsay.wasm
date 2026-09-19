#!/bin/sh

# Refresh the vendored Unicode Character Database files.
#
# Usage: fetch-ucd.sh <version> <directory>
#
# This is the only step that needs the network.
# unicode_tables.h is generated from these files and the break test reads one of them,
# so moving to a new Unicode version is this command plus the diff it produces.

set -eu

version=${1:?usage: fetch-ucd.sh <version> <directory>}
dir=${2:?usage: fetch-ucd.sh <version> <directory>}
base="https://www.unicode.org/Public/$version/ucd"

mkdir -p "$dir"
for path in \
  EastAsianWidth.txt \
  DerivedCoreProperties.txt \
  extracted/DerivedGeneralCategory.txt \
  emoji/emoji-data.txt \
  auxiliary/GraphemeBreakProperty.txt \
  auxiliary/GraphemeBreakTest.txt
do
  name=$(basename "$path")
  echo "$name: fetching $base/$path"
  curl -fsS --max-time 120 -o "$dir/$name" "$base/$path"
done
echo "ucd: $dir now holds Unicode $version"
