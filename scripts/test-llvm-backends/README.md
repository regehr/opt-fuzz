# Prereqs

You should be working on a recent version of OS X or Linux.

# Build alive2

- clone an LLVM, it should be the same version that you will build Anvill against

- build LLVM with exception handling and RTTI enabled; you can make this
  happen with a cmake command similar to this one:

```
cmake -G Ninja -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON -DLLVM_BUILD_LLVM_DYLIB=ON -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=ON -DCMAKE_INSTALL_PREFIX=$HOME/llvm-install ../llvm -DLLVM_ENABLE_PROJECTS="clang"
```

- clone [Alive2](https://github.com/AliveToolkit/alive2)

- configure Alive2 so that it builds against your new LLVM using a cmake command line similar to this one

```
cmake .. -G Ninja -DBUILD_TV=1 -DBUILD_LLVM_UTILS=1 -DLLVM_DIR=$HOME/llvm-install/lib/cmake/llvm -DCMAKE_BUILD_TYPE=Release
ninja
```

- run the Alive2 test suite and make sure everything works

```
ninja check
```

Also make sure you ended up with an executable called `alive-tv`, this
is the one you actually need.

# Build Anvill

Follow external instructions to do this. You must end up with an
executable called `anvill-decompile-json-10.0` (or something similar
to this, if you are using an LLVM version other than 10).

# Build opt-fuzz

Clone this repo and build opt-fuzz against the same version of LLVM
that you are using for alive2 and for Anvill.

# Fixup paths

The top-level program you will be running here is called
`check-file.pl`.  It is located in the same directory as this
README. Edit paths at the top of this file so that it can find your
opt-fuzz, alive2, and anvill executables.

This file also contains some variables that control which architecture
you are compiling to. If you want to use ARM instead of the default
x86-64, then you need to install an ARM toolchain and also adjust the
variables near the top of `check-file.pl` to refer to ARM stuff
instead.

# Run a simple test

Start with a very simple LLVM function such as `test1.ll` which is
located in the same directory as this README. Use `check-file.pl` to
verify that when it is compiled to x86-64 and then decompiled back to
LLVM, the final LLVM IR is a refinememnt of the original IR:

```
./check-file test1.ll
```

This should not produce much output to the terminal, but instead will
tell you about the results in a directory called `output`. The file
`output/test1.log` should contain this text at the end:

```
----------------------------------------
define i64 @slice(i64 %0, i64 %1) {
%2:
  %3 = add nsw i64 %1, %0
  ret i64 %3
}
=>
define i64 @slice(i64 %rdi, i64 %rsi) {
%0:
  %1 = add i64 %rsi, %rdi
  ret i64 %1
}
Transformation seems to be correct!

Summary:
  1 correct transformations
  0 incorrect transformations
  0 errors
```

If you see this, then you are all set. The key part of the output is
`1 correct transformations`. Looking at the IR before and after the
roundtrip through x86-64, we can see that the two functions actually
differ: the original code contains the `nsw` qualifier which was
inserted by Clang because signed overflow is undefined in C. The
lifted IR, on the other hand, lacks this qualifier because at the CPU
level the math is two's complement. The lifted code refines the
original code, but this transformation would not validate in the other
direction.

# Run a more interesting set of tests

Ok, now use opt-fuzz. In a temporary directory try this:

```
/path/to/opt-fuzz --fewconsts --promote=64 --one-func-per-file --width=64 --num-insns=1
```

This should produce no output on the command line, and it should leave
a bunch of IR files in the current directory. Right now 1224 files are
produced, but this can change at any time as opt-fuzz is modified.

Next, test all of these files:

```
ls *.ll | parallel /path/to/check-file.pl
```

This will use all your cores and will take a little while. When it
finishes, the `output` subdirectory should contain as many `.log`
files as you had IR files to start out with. For every log file that
does not contain the text `1 correct transformations`, something went
wrong. You will probably want some programmatic help in categorizing
these so they can be looked at efficiently. We have not written that
part yet.
