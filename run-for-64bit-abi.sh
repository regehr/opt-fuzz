N=1
OPTFUZZ=/home/regehr/opt-fuzz/build/opt-fuzz
ARGS='--cores=32 --fewconsts --promote=64'

$OPTFUZZ $ARGS --width=8 --num-insns=$N
$OPTFUZZ $ARGS --width=16 --num-insns=$N
$OPTFUZZ $ARGS --width=32 --num-insns=$N
$OPTFUZZ $ARGS --width=64 --num-insns=$N
$OPTFUZZ $ARGS --width=8 --num-insns=$N --geni1
$OPTFUZZ $ARGS --width=16 --num-insns=$N --geni1
$OPTFUZZ $ARGS --width=32 --num-insns=$N --geni1
$OPTFUZZ $ARGS --width=64 --num-insns=$N --geni1
