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

Prereq: Clang/LLVM 10 is installed and in the PATH. If it is not, you
can tell CMake where it is using the LLVM_DIR argument.

```
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
```

# TODO opt-fuzz short-term improvements

- write code for return values in memory
- add a separate in-memory mode where args/results are in a struct, pointer as argument
- generate vectors
  - width becomes element width
  - additional argument for vector size
- optionally print to pipes running compressors
- figure out optimal number of cores for max speedup

# TODO opt-fuzz longer term / less important improvements

- generate FP types
- generate pointers/GEPs/allocas/memcpys/etc.
- leave wide-bitwidth constants as symbolic, use Klee to cover
  interesting cases in the optimizer; or try AFL, but again only
  changing values of constants
- make forward branches work
- phi shouldn't use any budget?
- implement Nuno's ideas about synthesizing good constants from Alive preconditions,
  see his mail from May 20, 2017
- factor out budget > 0 checks

# TODO using opt-fuzz

- a crazy idea: use Alive to compare new implementations of an
  optimization (e.g. NewGVN vs GVN). We could use Alive to find test
  cases where the old implementation generates weaker code (i.e., with
  more UB/poison, meaning e.g. that it attaches more attributes and
  these are relevant)
