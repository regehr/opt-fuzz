set -e

BASE=all

# this has to be the native width used by opt-fuzz
WIDTH=32

# opt-fuzz emits too many arguments, get rid of unneeded ones
opt -strip ${BASE}.bc -S -o - | ~/alive2/scripts/test-llvm-backends/unused-arg-elimination.pl | opt -strip -S -o ${BASE}-stripped.ll

# IR -> object code
clang -c -O ${BASE}-stripped.ll -o ${BASE}.o

####################################################
# object code -> IR, run one of these

/home/regehr/retdec-install/bin/retdec-decompiler.py ${BASE}.o
mv all.o.ll all-decomp.ll

reopt --header=protos.h --llvm ${BASE}.o > ${BASE}-decomp.ll

/home/regehr/mctoll/llvm-project/build/bin/llvm-mctoll ${BASE}.o

####################################################

# ABI allows unused bits of return to be trashed; add an instruction
# masking these bits off. also strip out whatever register names the
# decompiler decided to use.
~/alive2/scripts/test-llvm-backends/maskret.pl $WIDTH < ${BASE}-decomp.ll | opt -strip -S -o ${BASE}-decomp2.ll

# translation validation
~/alive2/build/alive-tv ${BASE}-stripped.ll ${BASE}-decomp2.ll --disable-poison-input --disable-undef-input
