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

# Build Anvill

Follow external instructions to do this. You must end up with an
executable called anvill-decompile-json-10.0 (or similar if you use an
LLMV version other than 10).

# Build opt-fuzz

Clone this repo and build opt-fuzz against the same version of LLVM
that you are using for alive2 and for Anvill.

# Fixup paths

The top-level program you will be running here is called `check-file.pl`
