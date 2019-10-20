N=1
OPTFUZZ=/home/regehr/opt-fuzz/build/opt-fuzz
ARGS='--cores=32 --fewconsts --promote=64'

$OPTFUZZ $ARGS --width=8 --num-insns=$N --base="w8"
$OPTFUZZ $ARGS --width=16 --num-insns=$N --base="w16"
$OPTFUZZ $ARGS --width=32 --num-insns=$N --base="w32"
$OPTFUZZ $ARGS --width=64 --num-insns=$N --base="w64"
$OPTFUZZ $ARGS --width=8 --num-insns=$N --geni1 --base="w8r1"
$OPTFUZZ $ARGS --width=16 --num-insns=$N --geni1 --base="w16r1"
$OPTFUZZ $ARGS --width=32 --num-insns=$N --geni1 --base="w32r1"
$OPTFUZZ $ARGS --width=64 --num-insns=$N --geni1 --base="w64r1"

# add a version with args/rets in memory

