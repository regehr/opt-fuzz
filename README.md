# opt-fuzz

This is a simple implementation of bounded exhaustive testing for LLVM
programs. It is useful for testing optimizations. Although it could be
used in a variety of ways, we are using it in conjunction with Alive:

  https://github.com/nunoplopes/alive

The process is to generate a set of functions and then, for each:
- run one or more LLVM passes on the function
- use Alive to prove that the optimized code refines the original code (i.e.,
  that the optimization was correct)
Any verification failure represents a bug in LLVM or Alive.

# Building

Prereq: Clang/LLVM 4.0 is installed and in the PATH.

```
mkdir build
cd build
cmake .. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
```
