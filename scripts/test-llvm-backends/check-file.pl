#!/usr/bin/perl -w

use strict;
use autodie;
use File::Basename;

my $OPTFUZZ = $ENV{"HOME"}."/opt-fuzz";
my $WIDTH = 32;

my $ANVILL = $ENV{"HOME"}."/remill-build/tools/anvill/anvill-decompile-json-8.0";
my $ALIVE = $ENV{"HOME"}."/alive2/build/alive-tv";

my $SCRIPTS = "${OPTFUZZ}/scripts/test-llvm-backends";

my $inf = $ARGV[0];
die unless -f $inf;

my %suffixes = (".bc" => 1,
                ".ll" => 1);
my ($base, $dirs, $suffix) = fileparse($inf, keys %suffixes);
die "expected LLVM IR" unless exists $suffixes{$suffix};
$base = $dirs . $base;

# opt-fuzz emits too many arguments, get rid of unneeded ones
system "opt -strip $inf -S -o - | ${SCRIPTS}/unused-arg-elimination.pl | opt -strip -S -o ${base}-stripped.ll";

# IR -> object code
system "clang -w -c -O ${base}-stripped.ll -o ${base}.o";

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
    print "object code: $bytes\n";
    open $INF, "<${SCRIPTS}/slice.json" or die;
    open my $OUTF, ">slice2.json" or die;
    while (my $line = <$INF>) {
        $line =~ s/CODEGOESHERE/$bytes/;
        print $OUTF $line;
    }    
    close $INF;
    close $OUTF;
    system "${ANVILL} --spec slice2.json  --bc_out ${base}-decomp.bc >/dev/null 2>&1";
    open $INF, "llvm-dis ${base}-decomp.bc -o - |" or die;
    open $OUTF, "| opt -O2 -S -o - >${base}-decomp.ll" or die;
    while (my $line = <$INF>) {
        if (($line =~ /tail call/ &&
             $line =~ /remill_function_return/) ||
            $line =~ /target triple/) {
        } else {
            print $OUTF $line;
        }
    }
    close $INF;
    close $OUTF;
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

# ABI allows unused bits of return to be trashed; add an instruction
# masking these bits off. also strip out whatever register names the
# decompiler decided to use.
# system "~/alive2/scripts/test-llvm-backends/maskret.pl $WIDTH < ${base}-decomp.ll | opt -strip -S -o ${base}-decomp2.ll";

# translation validation
system "${ALIVE} ${base}-stripped.ll ${base}-decomp.ll --disable-poison-input --disable-undef-input";
