#!/usr/bin/perl -w

use strict;
use autodie;
use File::Basename;

my $OPTFUZZ=$ENV{"HOME"}."/opt-fuzz";
my $WIDTH = 32;

my $inf = $ARGV[0];
die unless -f $inf;

my @suffixes = (".bc", ".ll");
my ($base, $dirs, $suffix) = fileparse($inf, @suffixes);
die unless defined $suffix;
$base = $dirs . $base;

print "$base\n";
exit 0;

# opt-fuzz emits too many arguments, get rid of unneeded ones
system "opt -strip $inf -S -o - | ${OPTFUZZ}/scripts/test-llvm-backends/unused-arg-elimination.pl | opt -strip -S -o ${BASE}-stripped.ll";

# IR -> object code
system "clang -c -O ${BASE}-stripped.ll -o ${BASE}.o";

####################################################
# object code -> IR, run one of these

system "/home/regehr/retdec-install/bin/retdec-decompiler.py ${BASE}.o";
system "mv all.o.ll all-decomp.ll";

system "reopt --header=protos.h --llvm ${BASE}.o > ${BASE}-decomp.ll";

system "/home/regehr/mctoll/llvm-project/build/bin/llvm-mctoll ${BASE}.o";

####################################################

# ABI allows unused bits of return to be trashed; add an instruction
# masking these bits off. also strip out whatever register names the
# decompiler decided to use.
system "~/alive2/scripts/test-llvm-backends/maskret.pl $WIDTH < ${BASE}-decomp.ll | opt -strip -S -o ${BASE}-decomp2.ll";

# translation validation
system "~/alive2/build/alive-tv ${BASE}-stripped.ll ${BASE}-decomp2.ll --disable-poison-input --disable-undef-input";
