#!/usr/bin/perl -w

use strict;
use autodie;
use File::Basename;

my $OPTFUZZ = $ENV{"HOME"}."/opt-fuzz";
my $WIDTH = 32;

#my $ARCH = "x86-64";
#my $AS = "as";
#my $OBJDUMP = "objdump";

my $ARCH = "arm64";
my $AS = "aarch64-linux-gnu-as";
my $OBJDUMP = "aarch64-linux-gnu-objdump";

my $ANVILL = $ENV{"HOME"}."/remill-build/tools/anvill/anvill-decompile-json-8.0";

my $ALIVE = $ENV{"HOME"}."/alive2/build/alive-tv";
my $ALIVEFLAGS = "--disable-poison-input --disable-undef-input --smt-to=90000";

my $SCRIPTS = "${OPTFUZZ}/scripts/test-llvm-backends";

my $inf = $ARGV[0];
die unless -f $inf;

my %suffixes = (".bc" => 1,
                ".ll" => 1);
my ($base, $dirs, $suffix) = fileparse($inf, keys %suffixes);
die "expected LLVM IR" unless exists $suffixes{$suffix};
$base = $dirs . $base;

open my $LOG, ">${base}.log" or die;
print $LOG "==== checking $inf ====\n";
close $LOG;

# opt-fuzz emits too many arguments, get rid of unneeded ones
system "opt -strip $inf -S -o - | ${SCRIPTS}/unused-arg-elimination.pl | opt -strip -S -o ${base}-stripped.ll";

# IR -> object code
system "llc -march=${ARCH} ${base}-stripped.ll -o ${base}.s";
system "${AS} ${base}.s -o ${base}.o";
    
####################################################
# object code -> IR, run one of these

# ANVILL
if (1) {
    open my $INF, "${OBJDUMP} -d ${base}.o |" or die;
    my $bytes = "";

    if ($ARCH eq "x86-64") {
        while (my $line = <$INF>) {
            if ($line =~ /[0-9a-f]+:\s+(([0-9a-f][0-9a-f] )+)\s+/) {
                my $asm = $1;
                $asm =~ s/ //g;
                $bytes .= $asm;
            }
        }
        close $INF;
        # no sense proceeding if we didn't end up at a ret
        die unless substr($bytes, -2) eq "c3";
    } elsif ($ARCH eq "arm64") {
        while (my $line = <$INF>) {
            if ($line =~ /[0-9a-f]+:\s+([0-9a-f]{8})+\s+/) {
                my $asm = $1;
                $bytes .= $asm;
            }
        }
        close $INF;
    } else {
        die "unknown arch";
    }
    
    print "object code: $bytes\n";
    open $INF, "<${base}-stripped.ll" or die;
    my $nargs = 0;
    while (my $line = <$INF>) {
        if ($line =~ /define/ && $line =~ /slice/) {
            for (my $i = 0; $i < length($line); $i++) {
                $nargs++ if (substr($line, $i, 1) eq ",");
            }
        }
    }
    $nargs++;
    close $INF;
    print "detected $nargs function arguments\n";
    open $INF, "<${SCRIPTS}/${ARCH}-${nargs}arg.json" or die;
    open my $OUTF, ">${base}.json" or die;
    while (my $line = <$INF>) {
        $line =~ s/CODEGOESHERE/$bytes/;
        print $OUTF $line;
    }    
    close $INF;
    close $OUTF;
    system "${ANVILL} --spec ${base}.json  --bc_out ${base}-decomp.bc >${base}.log 2>&1";
    open $INF, "llvm-dis ${base}-decomp.bc -o - |" or die;
    open $OUTF, "| opt -O2 -S -o - >${base}-decomp.ll" or die;
    while (my $line = <$INF>) {
        next if ($line =~ /target triple/);
        if ($line =~ /(\%[0-9]+) = (tail )?call/) {
            my $var = $1;
            if ($line =~ /remill_function_return/) {
                print $OUTF "${var} = add i32 0, 0\n";
                next;
            }
            if ($line =~ /remill_error/) {
                die unless $line =~ s/%struct.State\*.*?,/%struct.State\* null,/;
                print $OUTF $line;
                next;
            }
        }
        print $OUTF $line;
    }
    close $INF;
    close $OUTF;
}

# MCTOLL
if (0) {
    system "llvm-mctoll ${base}.o -o - | opt -O2 -S -o ${base}-decomp.ll 2> ${base}.log";
}

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
system "${ALIVE} ${base}-stripped.ll ${base}-decomp.ll ${ALIVEFLAGS} >> ${base}.log 2>&1";
