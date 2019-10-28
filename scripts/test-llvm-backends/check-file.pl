#!/usr/bin/perl -w

use strict;
use autodie;
use File::Basename;

my $OPTFUZZ=$ENV{"HOME"}."/opt-fuzz";
my $WIDTH = 32;

my $inf = $ARGV[0];
die unless -f $inf;

my %suffixes = (".bc" => 1,
                ".ll" => 1);
my ($base, $dirs, $suffix) = fileparse($inf, keys %suffixes);
die "expected LLVM IR" unless exists $suffixes{$suffix};
$base = $dirs . $base;

# opt-fuzz emits too many arguments, get rid of unneeded ones
system "opt -strip $inf -S -o - | ${OPTFUZZ}/scripts/test-llvm-backends/unused-arg-elimination.pl | opt -strip -S -o ${base}-stripped.ll";

# IR -> object code
system "clang -c -O ${base}-stripped.ll -o ${base}.o";

####################################################
# object code -> IR, run one of these

# ANVILL
if (1) {
    open my $INF, "objdump -d ${base}.o |" or die;
    my $bytes = "";
    while (my $line = <$INF>) {
        if ($line =~ /[0-9a-f]+:\s+(([0-9a-f][0-9a-f] )+)\s+[a-zA-Z]/) {
            my $asm = $1;
            $asm =~ s/ //g;
            $bytes .= $asm;
        }
    }
    close $INF;
    print "$bytes\n";
}

# MCTOLL

# RETDEC
if (0) {
    system "/home/regehr/retdec-install/bin/retdec-decompiler.py ${base}.o";
    system "mv all.o.ll all-decomp.ll";
}

# REOPT
if (0) {
    system "reopt --header=protos.h --llvm ${base}.o > ${base}-decomp.ll";
}

####################################################

exit 0;

# ABI allows unused bits of return to be trashed; add an instruction
# masking these bits off. also strip out whatever register names the
# decompiler decided to use.
system "~/alive2/scripts/test-llvm-backends/maskret.pl $WIDTH < ${base}-decomp.ll | opt -strip -S -o ${base}-decomp2.ll";

# translation validation
system "~/alive2/build/alive-tv ${base}-stripped.ll ${base}-decomp2.ll --disable-poison-input --disable-undef-input";
