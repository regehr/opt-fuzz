N=2
OPTFUZZ=$HOME/opt-fuzz/build/opt-fuzz
ARGS='--cores=10 --fewconsts --promote=64 --one-func-per-file --use-intrinsics=false'

#$OPTFUZZ $ARGS --width=8 --num-insns=$N --base="w8_"
#$OPTFUZZ $ARGS --width=16 --num-insns=$N --base="w16_"
$OPTFUZZ $ARGS --width=32 --num-insns=$N --base="w32_"
#$OPTFUZZ $ARGS --width=64 --num-insns=$N --base="w64_"
#$OPTFUZZ $ARGS --width=8 --num-insns=$N --geni1 --base="w8r1_"
#$OPTFUZZ $ARGS --width=16 --num-insns=$N --geni1 --base="w16r1_"
#$OPTFUZZ $ARGS --width=32 --num-insns=$N --geni1 --base="w32r1_"
#$OPTFUZZ $ARGS --width=64 --num-insns=$N --geni1 --base="w64r1_"

# add a version with args/rets in memory

