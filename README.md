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

# TODO opt-fuzz improvements

- either print bitcode instead of llvm assembly or else print
  functions to named pipes running bzip2
- leave wide-bitwidth constants as symbolic, use Klee to cover
  interesting cases in the optimizer; or try AFL, but again only
  changing values of constants
- Debug branches
- randomize L and R when appropriate
- revive the parallel DFS work; goal is to use many cores w/o being a
  fork bomb; possible mechanisms
  * priorities
  * pipe/semaphore
  * OS-supported limit on number of processes
- phi shouldn't use any budget?
- maintain a separate vector of values for each width
- implement Nuno's ideas about synthesizing good constants from Alive preconditions,
  see his mail from May 20, 2017
- factor out budget > 0 checks
- remove unused arguments before printing
- look for cases where the folder fires despite us not wanting it to
- optionally test the folder
- flatten the choice tree so that opt-fuzz works better as a fuzzer;
  should suffice to do this just in GenVal
- Optionally generate only forward branches
- Generate x.with.overflow instructions and insertvalue/extractvalue
- Generate integer intrinsics, popcount and similar, the same ones we
  added to Souper
- Generate pointers/GEPs/allocas

# TODO using opt-fuzz

- UB flag inference
- leave wide-bitwidth constants as symbolic, use Klee to cover
  interesting cases in the optimizer; or try AFL, but again only
  changing values of constants
- a crazy idea: use Alive to compare new implementations of an
  optimization (e.g. NewGVN vs GVN). We could use Alive to find test
  cases where the old implementation generates weaker code (i.e., with
  more UB/poison, meaning e.g. that it attaches more attributes and
  these are relevant)
- should we have a buildbot doing this kind of testing? Run every so
  many days or at least before releases?
