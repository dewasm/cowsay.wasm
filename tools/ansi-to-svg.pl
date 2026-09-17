#!/usr/bin/env perl

# Render ANSI-colored terminal output, read from stdin, as an SVG terminal window.
#
# Usage: ansi-to-svg.pl [title] < output.txt > out.svg
#
# Only the escapes cowsay.wasm emits are understood: truecolor foreground, background, and resets.
# A full block (U+2588) becomes a filled rectangle rather than a glyph;
# the picture therefore does not depend on the viewer's font.

use strict;
use warnings;

# The block glyph must be one character, not its three UTF-8 bytes.
binmode STDIN, ":encoding(UTF-8)";
binmode STDOUT, ":encoding(UTF-8)";

my $title = shift // '';

my $CW = 9;          # cell width
my $LH = 19;         # line height
my $PAD = 16;        # padding around the text
my $BG = '#282c34';  # window background
my $FG = '#d8d5d0';  # default foreground
my $DIM = '#7f848e'; # the command line above the output

# Parse the input into a grid of cells, each [char, foreground, background].
my (@grid, $row, $col, $fg, $bg);
$row = $col = 0;
($fg, $bg) = (undef, undef);
local $/;
my $input = <STDIN>;
for my $part (split /(\e\[[0-9;]*m)/, $input) {
  next unless length $part;
  if ($part =~ /^\e\[([0-9;]*)m$/) {
    my @codes = split /;/, $1;
    @codes = (0) unless @codes;
    while (@codes) {
      my $code = shift @codes;
      if ($code == 0) { ($fg, $bg) = (undef, undef) }
      elsif ($code == 39) { $fg = undef }
      elsif ($code == 49) { $bg = undef }
      elsif (($code == 38 || $code == 48) && @codes >= 4 && $codes[0] == 2) {
        shift @codes;
        my $color = sprintf '#%02x%02x%02x', splice(@codes, 0, 3);
        $code == 38 ? ($fg = $color) : ($bg = $color);
      }
    }
    next;
  }
  for my $char (split //, $part) {
    if ($char eq "\n") { $row++; $col = 0; next }
    $grid[$row][$col] = [$char, $fg, $bg];
    $col++;
  }
}

my $rows = scalar @grid;
$rows++ if length $title;
my $cols = 0;
for my $line (@grid) {
  $cols = scalar @$line if $line && @$line > $cols;
}
$cols = length($title) + 2 if length($title) + 2 > $cols;
my $width = $PAD * 2 + $cols * $CW;
my $height = $PAD * 2 + $rows * $LH;

sub escape {
  my $s = shift;
  $s =~ s/&/&amp;/g;
  $s =~ s/</&lt;/g;
  $s =~ s/>/&gt;/g;
  return $s;
}

sub text_run { # <col> <row> <text> <fill>
  my ($c, $r, $text, $fill) = @_;
  my $x = $PAD + $c * $CW;
  my $y = $PAD + $r * $LH + 14;
  printf qq{  <text x="%d" y="%d" fill="%s" xml:space="preserve" textLength="%d" }
       . qq{lengthAdjust="spacingAndGlyphs">%s</text>\n},
    $x, $y, $fill, length($text) * $CW, escape($text);
}

sub fill_run { # <col> <row> <count> <fill>
  my ($c, $r, $n, $fill) = @_;
  printf qq{  <rect x="%d" y="%d" width="%d" height="%d" fill="%s"/>\n},
    $PAD + $c * $CW, $PAD + $r * $LH, $n * $CW, $LH, $fill;
}

my $font = 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
printf qq{<svg xmlns="http://www.w3.org/2000/svg" }
     . qq{width="%d" height="%d" viewBox="0 0 %d %d" }
     . qq{font-family="%s" font-size="15">\n},
  $width, $height, $width, $height, $font;
printf qq{  <rect width="%d" height="%d" rx="6" fill="%s"/>\n}, $width, $height, $BG;

my $offset = 0;
if (length $title) {
  text_run(0, 0, $title, $DIM);
  $offset = 1;
}

# A run of cells sharing a color becomes one rectangle or one text element.
for my $r (0 .. $#grid) {
  my $line = $grid[$r] // [];
  my $c = 0;
  while ($c < @$line) {
    my $cell = $line->[$c];
    unless ($cell) { $c++; next }
    my ($char, $cfg, $cbg) = @$cell;
    if ($char eq "\x{2588}" || defined $cbg) {
      # A block glyph paints in the foreground color, a colored space in the background color.
      my $fill = $char eq "\x{2588}" ? ($cfg // $FG) : $cbg;
      my $n = 0;
      while ($c + $n < @$line) {
        my $next = $line->[$c + $n] or last;
        my $next_fill = $next->[0] eq "\x{2588}" ? ($next->[1] // $FG) : $next->[2];
        last unless defined $next_fill && $next_fill eq $fill;
        $n++;
      }
      fill_run($c, $r + $offset, $n, $fill);
      $c += $n;
    } else {
      my $fill = $cfg // $FG;
      my $text = '';
      while ($c + length($text) < @$line) {
        my $next = $line->[$c + length $text] or last;
        last if $next->[0] eq "\x{2588}" || defined $next->[2];
        last unless ($next->[1] // $FG) eq $fill;
        $text .= $next->[0];
      }
      text_run($c, $r + $offset, $text, $fill) if $text =~ /\S/;
      $c += length $text;
    }
  }
}
print "</svg>\n";
