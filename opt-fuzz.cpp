//===-------- opt-fuzz.cpp - Generate LL files to stress-test LLVM --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This utility does bounded exhaustive generation of LLVM IR
// functions containing integer instructions. These can be used to
// stress-test different components of LLVM.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LegacyPassNameParser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include <algorithm>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace llvm;

namespace {

cl::opt<int> Cores("cores", cl::desc("How many cores to use (default=1)"),
                   cl::init(1));

cl::opt<int> W("width", cl::desc("Base integer width (default=2)"),
               cl::init(2));

cl::opt<int> N("num-insns", cl::desc("Number of instructions (default=2)"),
               cl::init(2));

cl::opt<int> Promote("promote",
                     cl::desc("Promote narrower arguments and return values to "
                              "this width (default=no promotion)"),
                     cl::init(-1));

cl::opt<bool>
    GenerateUndef("generate-undef",
                  cl::desc("Generate explicit undef inputs (default=false)"),
                  cl::init(false));

#if LLVM_VERSION_MAJOR >= 10
cl::opt<bool> GenerateFreeze("generate-freeze",
                             cl::desc("Generate freeze (default=true)"),
                             cl::init(true));
#endif

cl::opt<std::string>
    BaseName("base",
             cl::desc("Base name for emitted functions (default=\"func\")"),
             cl::init("func"));

cl::opt<bool>
    ArgsFromMem("args-from-memory",
                cl::desc("Function arguments come from memory instead of "
                         "calling convention (default=false)"),
                cl::init(false));

cl::opt<bool> RetToMem("return-to-memory",
                       cl::desc("Function return values go to memory instead "
                                "of calling convention (default=false)"),
                       cl::init(false));

cl::opt<bool>
    Branch("branches",
           cl::desc("Generate branches (default=false) (broken don't use)"),
           cl::init(false));

cl::opt<bool>
    UseIntrinsics("use-intrinsics",
                  cl::desc("Generate intrinsics like ctpop (default=true)"),
                  cl::init(true));

cl::opt<int> NumFiles("num-files",
                      cl::desc("Number of output files (default=1000)"),
                      cl::init(1000));

cl::opt<bool>
    OneFuncPerFile("one-func-per-file",
                   cl::desc("emit at most one function per output file, "
                            "subsumes --num-files (default=false)"),
                   cl::init(false));

cl::opt<bool> OneICmp("oneicmp",
                      cl::desc("Only emit one kind of icmp (default=false)"),
                      cl::init(false));

cl::opt<bool> OneBinop("onebinop",
                       cl::desc("Only emit one kind of binop (default=false)"),
                       cl::init(false));

cl::opt<bool> NoUB("noub",
                   cl::desc("Do not put UB flags on binops (default=false)"),
                   cl::init(false));

cl::opt<bool>
    Geni1("geni1",
          cl::desc("Functions return i1 instead of iN (default=false)"),
          cl::init(false));

cl::opt<bool>
    FewConsts("fewconsts",
              cl::desc("Instead of trying all values of every constant, try a "
                       "few selected constants (default=false)"),
              cl::init(false));

cl::opt<bool> Verify("verify", cl::desc("Run the LLVM verifier (default=true)"),
                     cl::init(true));

#define MAX_DEPTH 100

#undef assert
#define STRINGIFY(x) #x
#define assert(expr)                                                           \
  do {                                                                         \
    if (!(expr)) {                                                             \
      die(#expr " failed at line " STRINGIFY(__LINE__));                       \
      llvm_unreachable("assert");                                              \
    }                                                                          \
  } while (0)

struct shared {
  std::atomic_long NextId;
  pthread_mutex_t Lock;
  pthread_mutexattr_t LockAttr;
  pthread_cond_t Cond[MAX_DEPTH];
  int Waiting[MAX_DEPTH];
  pthread_condattr_t CondAttr;
  int Running;
  bool Stop;
} * Shmem;
std::string Choices;
long Id;

int Depth = 1;
bool Init = false;

void die(const char *str) {
  errs() << "ABORTING: " << str << "\n";
  if (Init) {
    // not checking return value here...
    pthread_mutex_lock(&Shmem->Lock);
    Shmem->Stop = true;
    for (int i = 0; i < MAX_DEPTH; ++i)
      pthread_cond_broadcast(&Shmem->Cond[i]);
    pthread_mutex_unlock(&Shmem->Lock);
  } else {
    Shmem->Stop = true;
  }
  exit(-1);
}

void decrease_runners(void) {
  if (pthread_mutex_lock(&Shmem->Lock) != 0)
    die("lock failed");

  assert(Shmem->Running <= Cores);

  Shmem->Running--;
  // FIXME could cache the max depth, perhaps don't care
  for (int i = MAX_DEPTH - 1; i >= 0; --i) {
    if (Shmem->Waiting[i] != 0) {
      Shmem->Waiting[i]--;
      if (pthread_cond_signal(&Shmem->Cond[i]) != 0)
        die("pthread_cond_signal failed");
      break;
    }
  }

  if (pthread_mutex_unlock(&Shmem->Lock) != 0)
    die("unlock failed");
}

void increase_runners(int Depth) {
  if (pthread_mutex_lock(&Shmem->Lock) != 0)
    die("lock failed");

  if (Depth >= MAX_DEPTH)
    die("oops, you'll need to rebuild opt-fuzz with a larger MAX_DEPTH");
  assert(Shmem->Running <= Cores);

  while (Shmem->Running >= Cores) {
    Shmem->Waiting[Depth]++;
    if (Shmem->Stop) {
      pthread_mutex_unlock(&Shmem->Lock);
      exit(-1);
    }
    if (pthread_cond_wait(&Shmem->Cond[Depth], &Shmem->Lock))
      die("pthread_cond_wait failed");
    if (Shmem->Stop) {
      pthread_mutex_unlock(&Shmem->Lock);
      exit(-1);
    }
  }
  Shmem->Running++;

  if (pthread_mutex_unlock(&Shmem->Lock) != 0)
    die("unlock failed");
}

int Choose(int n) {
  assert(n > 0);
  for (int i = 0; i < (n - 1); ++i) {
    if (Shmem->Stop) {
      pthread_mutex_unlock(&Shmem->Lock);
      exit(-1);
    }
    int ret = ::fork();
    if (ret == -1)
      die("fork failed");
    if (ret == 0) {
      // child
      Id = Shmem->NextId.fetch_add(1);
      Choices += std::to_string(i) + " ";
      ++Depth;
      ::srand(::getpid());
      return i;
    }
    // parent
    increase_runners(Depth);
    waitpid(-1, 0, WNOHANG);
  }
  Choices += std::to_string(n - 1) + " ";
  return n - 1;
}

IRBuilder<NoFolder> *Builder;
LLVMContext C;
std::vector<Value *> Vals;
Function *F;
Module *M;
std::vector<Value *> Args;
std::set<Value *> UsedArgs;
std::vector<BasicBlock *> BBs;
std::vector<BranchInst *> Branches;

Value *genVal(int &Budget, int Width, bool ConstOK, bool ArgOK = true);

void gen2(Value *&L, Value *&R, int &Budget, int Width) {
  L = genVal(Budget, Width, true);
  R = genVal(Budget, Width, !isa<Constant>(L) && !isa<UndefValue>(L));
  if ((rand() & 1) == 0) {
    Value *T = L;
    L = R;
    R = T;
  }
}

std::vector<Value *> gen3(int &Budget, int Width) {
  auto A = genVal(Budget, Width, true);
  auto B = genVal(Budget, Width, true);
  auto C = genVal(Budget, Width,
                  (!isa<Constant>(A) && !isa<UndefValue>(A)) ||
                      (!isa<Constant>(B) && !isa<UndefValue>(B)));
  switch (rand() % 5) {
  case 0:
    return std::vector{A, B, C};
  case 1:
    return std::vector{A, C, B};
  case 2:
    return std::vector{B, A, C};
  case 3:
    return std::vector{B, C, A};
  case 4:
    return std::vector{C, A, B};
  case 5:
    return std::vector{C, B, A};
  }
  assert(false);
}

// true pseudorandom, not BET
APInt randAPInt(int Width) {
  APInt Val(Width, 0);
  for (int i = 0; i < Width; ++i) {
    Val <<= 1;
    if (rand() < (RAND_MAX / 2))
      Val |= 1;
  }
  return Val;
}

bool okForBitIntrinsic(int W) {
  return W == 8 || W == 16 || W == 32 || W == 64 || W == 128 || W == 256;
}

Value *genVal(int &Budget, int Width, bool ConstOK, bool ArgOK) {
  if (Branch && Budget > 0 && Choose(2)) {
    --Budget;
    Value *V = Builder->CreatePHI(Type::getIntNTy(C, Width), N);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Branch && Budget > 0 && Budget != N && Choose(2)) {
    --Budget;
    BranchInst *Br;
    if (0 && Builder->GetInsertBlock()->size() > 0 && Choose(2)) {
      Br = Builder->CreateBr(BBs.at(0));
    } else {
      Value *C = genVal(Budget, 1, false, ArgOK);
      Br = Builder->CreateCondBr(C, BBs[0], BBs[0]);
    }
    Branches.push_back(Br);
    BasicBlock *BB = BasicBlock::Create(C, "", F);
    BBs.push_back(BB);
    Builder->SetInsertPoint(BB);
    auto V = genVal(Budget, Width, ConstOK, ArgOK);
    assert(V);
    return V;
  }

  if (UseIntrinsics && Budget > 0 && Width == W && okForBitIntrinsic(Width) &&
      Choose(2)) {
    --Budget;
    std::vector<Value *> A;
    std::vector<Type *> T;
    A.push_back(genVal(Budget, Width, false));
    T.push_back(A.at(0)->getType());
    Intrinsic::ID ID;
    switch (Choose(6)) {
    case 0:
      ID = Intrinsic::ctpop;
      break;
    case 1:
      if (Width != 16 && Width != 32 && Width != 64)
        exit(0);
      ID = Intrinsic::bitreverse;
      break;
    case 2:
      if (Width != 16 && Width != 32 && Width != 64)
        exit(0);
      ID = Intrinsic::bswap;
      break;
    case 3:
      A.push_back(Builder->getInt1(Choose(2)));
      ID = Intrinsic::ctlz;
      break;
    case 4:
      A.push_back(Builder->getInt1(Choose(2)));
      ID = Intrinsic::cttz;
      break;
    case 5:
      A.push_back(Builder->getInt1(Choose(2)));
      ID = Intrinsic::abs;
      break;
    }
    Value *V = Builder->CreateIntrinsic(ID, T, A);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    --Budget;
    Value *L, *R;
    gen2(L, R, Budget, Width);
    Value *C = genVal(Budget, 1, false);
    Value *V = Builder->CreateSelect(C, L, R);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == 1 && Choose(2)) {
    --Budget;
    Value *L, *R;
    gen2(L, R, Budget, W);
    CmpInst::Predicate P;
    switch (OneICmp ? 0 : Choose(10)) {
    case 0:
      P = CmpInst::ICMP_EQ;
      break;
    case 1:
      P = CmpInst::ICMP_NE;
      break;
    case 2:
      P = CmpInst::ICMP_UGT;
      break;
    case 3:
      P = CmpInst::ICMP_UGE;
      break;
    case 4:
      P = CmpInst::ICMP_ULT;
      break;
    case 5:
      P = CmpInst::ICMP_ULE;
      break;
    case 6:
      P = CmpInst::ICMP_SGT;
      break;
    case 7:
      P = CmpInst::ICMP_SGE;
      break;
    case 8:
      P = CmpInst::ICMP_SLT;
      break;
    case 9:
      P = CmpInst::ICMP_SLE;
      break;
    }
    Value *V = Builder->CreateICmp(P, L, R);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    int OldW = Width * 2;
    --Budget;
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, false),
                                    Type::getIntNTy(C, Width));
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == 1 && Choose(2)) {
    int OldW = W;
    --Budget;
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, false),
                                    Type::getIntNTy(C, 1));
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    int OldW = Width / 2;
    if (OldW > 1 && Choose(2))
      OldW = 1;
    --Budget;
    Value *V;
    if (Choose(2))
      V = Builder->CreateZExt(genVal(Budget, OldW, false),
                              Type::getIntNTy(C, Width));
    else
      V = Builder->CreateSExt(genVal(Budget, OldW, false),
                              Type::getIntNTy(C, Width));
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    --Budget;
    Instruction::BinaryOps Op;
    switch (OneBinop ? 0 : Choose(13)) {
    case 0:
      Op = Instruction::Add;
      break;
    case 1:
      Op = Instruction::Sub;
      break;
    case 2:
      Op = Instruction::Mul;
      break;
    case 3:
      Op = Instruction::SDiv;
      break;
    case 4:
      Op = Instruction::UDiv;
      break;
    case 5:
      Op = Instruction::SRem;
      break;
    case 6:
      Op = Instruction::URem;
      break;
    case 7:
      Op = Instruction::And;
      break;
    case 8:
      Op = Instruction::Or;
      break;
    case 9:
      Op = Instruction::Xor;
      break;
    case 10:
      Op = Instruction::Shl;
      break;
    case 11:
      Op = Instruction::AShr;
      break;
    case 12:
      Op = Instruction::LShr;
      break;
    }
    Value *L, *R;
    gen2(L, R, Budget, Width);
    Value *V = Builder->CreateBinOp(Op, L, R);
    if (!NoUB) {
      if ((Op == Instruction::Add || Op == Instruction::Sub ||
           Op == Instruction::Mul || Op == Instruction::Shl) &&
          Choose(2)) {
        BinaryOperator *B = cast<BinaryOperator>(V);
        B->setHasNoSignedWrap(true);
      }
      if ((Op == Instruction::Add || Op == Instruction::Sub ||
           Op == Instruction::Mul || Op == Instruction::Shl) &&
          Choose(2)) {
        BinaryOperator *B = cast<BinaryOperator>(V);
        B->setHasNoUnsignedWrap(true);
      }
      if ((Op == Instruction::UDiv || Op == Instruction::SDiv ||
           Op == Instruction::LShr || Op == Instruction::AShr) &&
          Choose(2)) {
        BinaryOperator *B = cast<BinaryOperator>(V);
        B->setIsExact(true);
      }
    }
    assert(V);
    Vals.push_back(V);
    return V;
  }

  if (UseIntrinsics && Budget > 0 && Width == W && Choose(2)) {
    --Budget;
    std::vector<Value *> Args = gen3(Budget, Width);
    Intrinsic::ID ID = Choose(2) ? Intrinsic::fshl : Intrinsic::fshr;
    std::vector<Type *> T{Type::getIntNTy(C, Width)};
    Value *V = Builder->CreateIntrinsic(ID, T, Args);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  // this one is a bit different than other instructions since we'll
  // synthesize it when either a full-width value or an i1 is required
  if (UseIntrinsics && Budget > 0 && (Width == 1 || Width == W) && Choose(2)) {
    --Budget;
    Value *L, *R;
    gen2(L, R, Budget, W);
    Intrinsic::ID ID;
    switch (Choose(6)) {
    case 0:
      ID = Intrinsic::uadd_with_overflow;
      break;
    case 1:
      ID = Intrinsic::sadd_with_overflow;
      break;
    case 2:
      ID = Intrinsic::usub_with_overflow;
      break;
    case 3:
      ID = Intrinsic::ssub_with_overflow;
      break;
    case 4:
      ID = Intrinsic::umul_with_overflow;
      break;
    case 5:
      ID = Intrinsic::smul_with_overflow;
      break;
    }
    Value *V = Builder->CreateBinaryIntrinsic(ID, L, R);
    assert(V);
    Value *V1 = Builder->CreateExtractValue(V, 0);
    Value *V2 = Builder->CreateExtractValue(V, 1);
    assert(V1);
    assert(V2);
    Vals.push_back(V1);
    Vals.push_back(V2);
    if (Width == 1)
      return V2;
    else
      return V1;
  }

  if (UseIntrinsics && Budget > 0 && Width == W && Choose(2)) {
    --Budget;
    Intrinsic::ID ID;
    switch (Choose(10)) {
    case 0:
      ID = Intrinsic::uadd_sat;
      break;
    case 1:
      ID = Intrinsic::usub_sat;
      break;
    case 2:
      ID = Intrinsic::sadd_sat;
      break;
    case 3:
      ID = Intrinsic::ssub_sat;
      break;
    case 4:
      ID = Intrinsic::smax;
      break;
    case 5:
      ID = Intrinsic::smin;
      break;
    case 6:
      ID = Intrinsic::umax;
      break;
    case 7:
      ID = Intrinsic::umin;
      break;
    case 8:
      ID = Intrinsic::sshl_sat;
      break;
    case 9:
      ID = Intrinsic::ushl_sat;
      break;
    default:
      llvm::report_fatal_error("oops");
    }
    Value *L, *R;
    gen2(L, R, Budget, Width);
    Value *V = Builder->CreateBinaryIntrinsic(ID, L, R);
    assert(V);
    Vals.push_back(V);
    return V;
  }

  // TODO: add fixed point intrinsics?

#if LLVM_VERSION_MAJOR >= 10
  if (Width == W && GenerateFreeze && Budget > 0 && Choose(2)) {
    --Budget;
    return Builder->CreateFreeze(genVal(Budget, W, false));
  }
#endif

  /*
   * from this point on we're not generating instructions and hence
   * not consuming budget
   */

  if (ConstOK && Choose(2)) {
    if (FewConsts) {
      int n = Choose(GenerateUndef ? 9 : 8);
      switch (n) {
      case 0:
        return ConstantInt::get(C, randAPInt(W));
      case 1:
        return ConstantInt::get(C, APInt(Width, -1));
      case 2:
        return ConstantInt::get(C, APInt(Width, 0));
      case 3:
        return ConstantInt::get(C, APInt(Width, 1));
      case 4:
        return ConstantInt::get(C, APInt(Width, 2));
      case 5:
        return ConstantInt::get(C, APInt::getSignedMaxValue(Width));
      case 6:
        return ConstantInt::get(C, APInt::getSignedMinValue(Width));
      case 7:
      again : {
        auto i = APInt(Width, (rand() % (10 + (2 * Width))) - (5 + Width));
        if (i == -1 || i == 0 || i == 1 || i == 2)
          goto again;
        return ConstantInt::get(C, i);
      }
      case 8:
        return UndefValue::get(Type::getIntNTy(C, Width));
      default:
        assert(false);
      }
      return ConstantInt::get(C, APInt(Width, 1));
    } else {
      int n = Choose((1 << Width) + (GenerateUndef ? 1 : 0));
      if (n == (1 << Width))
        return UndefValue::get(Type::getIntNTy(C, Width));
      else
        return ConstantInt::get(C, APInt(Width, n));
    }
  }

  if (ArgOK && Choose(2)) {
    /*
     * refer to a function argument; the function arguments are
     * pre-populated because it's hard to change a function signature
     * in LLVM
     *
     * there's a bit of extra complixity here because we don't want to
     * gratuitously use the different function arguments just to use
     * them -- we only want to choose among those that have already
     * been used + the first not-yet-used one (among those with
     * matching widths)
     */
    std::vector<Value *> Vs;
    bool found = false;
    for (auto a : Args) {
      if (a->getType()->getPrimitiveSizeInBits() != (unsigned)Width)
        continue;
      Vs.push_back(a);
      if (UsedArgs.find(a) == UsedArgs.end()) {
        UsedArgs.insert(a);
        found = true;
        break;
      }
    }
    /*
     * this isn't supposed to happen since we attempt to pre-populate
     * the function arguments conservatively
     */
    if (!found)
      errs() << "Error: ran out of function arguments of width " << Width
             << "\n";
    return Vs.at(Choose(Vs.size()));
  }

  std::vector<Value *> Vs;
  for (auto &it : Vals)
    if (it->getType()->getPrimitiveSizeInBits() == (unsigned)Width)
      Vs.push_back(it);
  // this can happen when no values have been created yet, no big deal
  if (Vs.size() == 0)
    exit(0);
  return Vs.at(Choose(Vs.size()));
}

BasicBlock *chooseTarget(BasicBlock *Avoid = 0) {
  std::vector<inst_iterator> targets;
  auto i = inst_begin(F), ie = inst_end(F);
  ++i;
  for (; i != ie; ++i)
    if (!i->isTerminator())
      targets.push_back(i);
  auto t = targets[Choose(targets.size())];
  Instruction *I = &*t;
  BasicBlock *BB;
  if (I == &*I->getParent()->getFirstInsertionPt()) {
    BB = I->getParent();
  } else {
    BB = I->getParent()->splitBasicBlock(I, "spl");
  }
  return BB;
}

std::vector<Value *> globs;

void makeArg(int W, std::vector<Type *> &ArgsTy,
             std::vector<Type *> &RealArgsTy) {
  int RealW = W;
  if (Promote != -1 && Promote > W)
    RealW = Promote;
  ArgsTy.push_back(IntegerType::getIntNTy(C, W));
  auto T = IntegerType::getIntNTy(C, RealW);
  RealArgsTy.push_back(T);
  if (ArgsFromMem) {
    GlobalVariable *g =
        new GlobalVariable(*M, T, /*isConstant=*/false,
                           /*Linkage=*/GlobalValue::ExternalLinkage,
                           /*Initializer=*/0);
    globs.push_back(g);
  }
}

void addArg(Value *a, int W, std::vector<Type *> &ArgsTy) {
  assert(a);
  if (Promote != -1 && Promote > W)
    Args.push_back(Builder->CreateTrunc(a, IntegerType::getIntNTy(C, W)));
  else
    Args.push_back(a);
}

void generate() {
  M = new Module("", C);
  std::vector<Type *> ArgsTy, RealArgsTy, MT;
  for (int i = 0; i < N + 2; ++i) {
    makeArg(W, ArgsTy, RealArgsTy);
    makeArg(W, ArgsTy, RealArgsTy);
    makeArg(1, ArgsTy, RealArgsTy);
    makeArg(W / 2, ArgsTy, RealArgsTy);
    makeArg(W * 2, ArgsTy, RealArgsTy);
  }
  int RetWidth = Geni1 ? 1 : W;
  if (Promote != -1 && Promote > RetWidth)
    RetWidth = Promote;
  auto FuncTy = FunctionType::get(Type::getIntNTy(C, RetWidth),
                                  ArgsFromMem ? MT : RealArgsTy, 0);
  F = Function::Create(FuncTy, GlobalValue::ExternalLinkage, BaseName, M);
  BBs.push_back(BasicBlock::Create(C, "", F));
  Builder = new IRBuilder<NoFolder>(BBs[0]);
  int Budget = N;
  Builder->SetInsertPoint(BBs[0]);

  if (ArgsFromMem) {
    for (unsigned i = 0; i < ArgsTy.size(); ++i) {
      Value *a = Builder->CreateLoad(RealArgsTy.at(i), globs.at(i));
      int W = ArgsTy.at(i)->getPrimitiveSizeInBits();
      addArg(a, W, ArgsTy);
    }
  } else {
    unsigned i = 0;
    for (auto it = F->arg_begin(); it != F->arg_end(); ++i, ++it) {
      int W = ArgsTy.at(i)->getPrimitiveSizeInBits();
      addArg(it, W, ArgsTy);
    }
  }

  // the magic happens in genVal()
  Value *V;
  if (Geni1)
    V = genVal(Budget, 1, false, false);
  else
    V = genVal(Budget, W, false, false);

  if ((unsigned)RetWidth > V->getType()->getPrimitiveSizeInBits())
    V = Builder->CreateZExt(V, IntegerType::getIntNTy(C, Promote));
  // terminate the only non-terminated BB
  Builder->CreateRet(V);

  // fixup branch targets
  for (auto &bi : Branches) {
    BasicBlock *BB1 = chooseTarget();
    bi->setSuccessor(0, BB1);
    if (bi->isConditional())
      bi->setSuccessor(1, chooseTarget(BB1));
  }

// finally, fixup the Phis -- first by splitting any BBs where a non-Phi
// precedes a Phi
redo:
  for (auto &bb : *F) {
    bool notphi = false;
    for (auto &i : bb) {
      if (!isa<PHINode>(i))
        notphi = true;
      if (notphi && isa<PHINode>(i)) {
        i.getParent()->splitBasicBlock(&i, "phisp");
        goto redo;
      }
    }
  }

  // and second by giving them incoming edges
  for (auto p = inst_begin(F), pe = inst_end(F); p != pe; ++p) {
    PHINode *P = dyn_cast<PHINode>(&*p);
    if (!P)
      continue;
    BasicBlock *BB = P->getParent();
    for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
      BasicBlock *Pred = *PI;
      assert(Budget == 0);
      Value *V =
          genVal(Budget, P->getType()->getPrimitiveSizeInBits(), true, false);
      P->addIncoming(V, Pred);
    }
  }

  // drop any program where we have a non-entry BB that lacks predecessors;
  // would be better to avoid creating these in the first place
  bool first = true;
  for (auto &bb : *F) {
    if (first) {
      first = false;
    } else {
      int p = 0;
      for (pred_iterator PI = pred_begin(&bb), E = pred_end(&bb); PI != E; ++PI)
        p++;
      if (p == 0) {
        // under what circumstances can this happen?
        exit(0);
      }
    }
  }
}

void output() {
  std::string SStr;
  raw_string_ostream SS(SStr);
  legacy::PassManager Passes;
  if (Verify)
    Passes.add(createVerifierPass());
  // Passes.add(createDeadCodeEliminationPass());
  Passes.add(createPrintModulePass(SS));
  Passes.run(*M);

  std::string func = SS.str();

  int fd;
  if (OneFuncPerFile) {
    std::stringstream ss;
    ss << BaseName << std::to_string(Id);
    func.replace(func.find(BaseName), BaseName.length(), "f");
    std::string FN = BaseName + std::to_string(Id) + ".ll";
    fd = open(FN.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IREAD | S_IWRITE);
  } else {
    std::stringstream ss;
    ss << BaseName << std::to_string(Id);
    func.replace(func.find(BaseName), BaseName.length(), ss.str());
    std::string FN = std::to_string(rand() % NumFiles) + ".ll";
    fd = open(FN.c_str(), O_RDWR | O_CREAT | O_APPEND, S_IREAD | S_IWRITE);
  }
  if (fd < 2)
    die("open failed");

  /*
   * hack -- instead of locking the file we're just going to count on
   * an atomic write and bail if it doesn't work -- this seems to work
   * fine on Linux and OS X
   */
  unsigned res = write(fd, func.c_str(), func.length());
  if (res != func.length())
    die("non-atomic write");
  res = close(fd);
  assert(res == 0);
}

} // namespace

int main(int argc, char **argv) {
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  if (W < 2)
    die("Width must be >= 2");

  Shmem =
      (struct shared *)::mmap(0, sizeof(struct shared), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANON, -1, 0);
  if (Shmem == MAP_FAILED)
    die("mmap failed");
  Shmem->NextId = 1;
  Shmem->Running = 1;
  if (pthread_mutexattr_init(&Shmem->LockAttr) != 0)
    die("pthread_mutexattr_init failed");
  if (pthread_mutexattr_setpshared(&Shmem->LockAttr, PTHREAD_PROCESS_SHARED) !=
      0)
    die("pthread_mutexattr_setpshared failed");
  if (pthread_mutex_init(&Shmem->Lock, &Shmem->LockAttr) != 0)
    die("pthread_mutex_init failed");
  if (pthread_condattr_init(&Shmem->CondAttr) != 0)
    die("pthread_condattr_init failed");
  if (pthread_condattr_setpshared(&Shmem->CondAttr, PTHREAD_PROCESS_SHARED) !=
      0)
    die("pthread_condattr_setpshared failed");
  for (int i = 0; i < MAX_DEPTH; ++i) {
    if (pthread_cond_init(&Shmem->Cond[i], &Shmem->CondAttr) != 0)
      die("pthread_cond_init failed");
    Shmem->Waiting[i] = 0;
  }
  Init = 1;

  int p[2];
  pid_t original_pid = ::getpid();
  if (::atexit(decrease_runners) != 0)
    die("atexit failed");
  /*
   * use a trick from stackoverflow to work around the fact that in
   * UNIX we can only wait on our children, not our extended
   * descendents: all descendents are going to inherit this pipe,
   * implicitly closing its fds when they terminate. at that point
   * reading from the pipe will not block but rather return with EOF
   */
  if (::pipe(p) != 0)
    die("pipe failed??");

  generate();
  output();

  if (::getpid() == original_pid) {
    char buf[1];
    ::close(p[1]);
    ::read(p[0], buf, 1);
    for (int i = 0; i < MAX_DEPTH; i++) {
      if (Shmem->Waiting[i] != 0)
        errs() << "oops, there are waiting processes at " << i << "\n";
    }
  }

  return 0;
}
