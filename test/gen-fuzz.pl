#!/usr/bin/env perl

# Deterministic fuzz case generator for the differential test.
#
# Usage: gen-fuzz.pl <outdir>
#
# Each case is a directory of two files.
# "argv" holds NUL-separated arguments, possibly empty; "stdin" the bytes fed to standard input.

use strict;
use warnings;

my $outdir = shift or die "usage: gen-fuzz.pl <outdir>\n";
srand(42);

my @charset = ('a' .. 'z', 'A' .. 'Z', '0' .. '9', '.', ',', '-', '!', '?', "'", '"', '(', ')');
my @flagpool = qw(-b -d -g -p -s -t -w -y);

sub word {
  my $len = 1 + int(rand(rand() < 0.15 ? 90 : 12));
  return join '', map { $charset[int(rand(@charset))] } 1 .. $len;
}

sub write_case {
  my ($n, $argv, $stdin) = @_;
  my $dir = sprintf '%s/%03d', $outdir, $n;
  mkdir $dir or die "mkdir $dir: $!";
  open my $a, '>', "$dir/argv" or die $!;
  print $a join("\0", @$argv);
  print $a "\0" if @$argv;
  close $a;
  open my $s, '>', "$dir/stdin" or die $!;
  print $s $stdin;
  close $s;
}

my $n = 0;
for (1 .. 150) {
  my @args;
  # Widths below 2 and a first word of "0" are deliberate fixes, so they diverge from the reference.
  # The fixed cases in run.sh cover them; the fuzz stays inside the specification.
  push @args, '-W', 2 + int(rand(88)) if rand() < 0.5;
  push @args, $flagpool[int(rand(@flagpool))] if rand() < 0.3;
  push @args, '-e', word() if rand() < 0.2;
  push @args, '-T', word() if rand() < 0.2;
  my @words = map { word() } 1 .. int(rand(10));
  $words[0] = word() while @words && $words[0] eq '0';
  push @args, @words;
  write_case($n++, \@args, '');
}
for (1 .. 100) {
  my @args;
  push @args, '-W', 2 + int(rand(88)) if rand() < 0.5;
  push @args, '-n' if rand() < 0.3;
  my $stdin = '';
  for (1 .. int(rand(8))) {
    my $line = '';
    $line .= ' ' x int(rand(4)) if rand() < 0.3;
    $line .= join(rand() < 0.2 ? "\t" : ' ', map { word() } 0 .. int(rand(6)));
    $line .= ' ' x int(rand(3)) if rand() < 0.2;
    $stdin .= "$line\n";
  }
  $stdin .= word() if rand() < 0.2; # final line without newline
  write_case($n++, \@args, $stdin);
}
print "$n\n";
