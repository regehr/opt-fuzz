#!/usr/bin/perl -w

use strict;
use autodie;
use File::Basename;

my $IND = $ARGV[0];
my $OUTD = $ARGV[1];

die "input directory doesn't exist?" unless -d $IND;
die "output directory doesn't exist?" unless -d $OUTD;

my @files = glob "$IND/*.ll";
print "processing ".scalar(@files)." files\n";

foreach my $inf (@files) {
    print "about to process $inf\n";
    my $outf = File::Basename::basename($inf, ".ll");
    open my $INF, "<$inf" or die;
    open my $OUTF, "| llvm-as -o $OUTD/${outf}.bc" or die;
    my %decs = ();
    while (my $line = <$INF>) {
        next if $line =~ /^; /;
        next if $line =~ /^attributes /;
        if ($line =~ /^declare / ||
	    $line =~ /external global/) {
            next if (exists $decs{$line});
            $decs{$line} = 1;
        }
        print $OUTF $line;
    }
    close $INF;
    close $OUTF;
}
