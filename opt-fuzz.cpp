//===-- opt-fuzz.cpp - Generate random LL files to stress-test LLVM ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that generates random .ll files to stress-test
// different components in LLVM.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassNameParser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ToolOutputFile.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
using namespace llvm;

static const unsigned W = 4; // width
static const int N = 5;      // number of instructions to generate
static const int NumFiles = 1000;

static const int Cpus = 4;

static cl::opt<bool> OneICmp("oneicmp", cl::desc("Only emit one kind of icmp"),
                             cl::init(false));
static cl::opt<bool> OneBinop("onebinop",
                              cl::desc("Only emit one kind of binop"),
                              cl::init(false));
static cl::opt<bool> NoUB("noub", cl::desc("Do not put UB flags on binops"),
                          cl::init(false));
static cl::opt<bool> OneConst("oneconst",
                              cl::desc("Only use one constant value"),
                              cl::init(false));
static cl::opt<bool> All("all", cl::desc("Generate all programs"),
                         cl::init(false));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<int> Seed("seed", cl::desc("PRNG seed"), cl::init(INT_MIN));
static cl::opt<std::string> ForcedChoiceStr("choices",
                                            cl::desc("Force these choices"));
static cl::opt<bool> Verify("verify", cl::desc("Run the LLVM verifier"),
                            cl::init(true));

static std::vector<int> ForcedChoices;

struct shared {
  std::atomic_long Children;
  std::atomic_long NextId;
} * Shmem;
static std::string Choices;
static long Id;

/*
 * custom assert handler since it's critical we decrease the process count
 * before exiting
 */
static int __check_handler(const char *exp, const char *file, const int line) {
  std::string err = "Assertion `" + std::string(exp) + "` failed at line " +
                    std::to_string(line) + " of file " + std::string(file) +
                    " with choices: " + Choices + "\n";
  errs() << err;
  --Shmem->Children;
  exit(-1);
}

#define check(x) ((void)(!(x) && __check_handler(#x, __FILE__, __LINE__)))

static unsigned Choose(unsigned n) {
  check(n > 0);
  if (All) {
    for (unsigned i = 0; i < (n - 1); ++i) {
      if (Shmem->Children >= Cpus)
        ::wait(0);
      int ret = ::fork();
      check(ret != -1);
      if (ret == 0) {
        Id = Shmem->NextId.fetch_add(1);
        ++Shmem->Children;
        Choices += std::to_string(i) + " ";
        return i;
      }
    }
    Choices += std::to_string(n - 1) + " ";
    return n - 1;
  } else if (!ForcedChoiceStr.empty()) {
    static unsigned Choice = 0;
    return ForcedChoices[Choice++];
  } else {
    unsigned i = rand() % n;
    Choices += std::to_string(i) + " ";
    return i;
  }
}

static IRBuilder<true, NoFolder> *Builder;
static LLVMContext *C;
static std::vector<Value *> Vals;
static Function *F;
static std::set<Argument *> UsedArgs;
static std::vector<BasicBlock *> BBs;
static std::vector<BranchInst *> Branches;

static void Done(void) {
  --Shmem->Children;
  exit(0);
}

static Value *genVal(int &Budget, unsigned Width, bool ConstOK,
                     bool ArgOK = true);

static void genLR(Value *&L, Value *&R, int &Budget, unsigned Width) {
  L = genVal(Budget, Width, true);
  bool Lconst = isa<Constant>(L) || isa<UndefValue>(L);
  R = genVal(Budget, Width, !Lconst);
}

static Value *genVal(int &Budget, unsigned Width, bool ConstOK, bool ArgOK) {
  if (Budget > 0 && Choose(2)) {
    if (Verbose)
      errs() << "adding a phi, budget = " << Budget << "\n";
    --Budget;
    Value *V = Builder->CreatePHI(Type::getIntNTy(*C, Width), N);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Budget != N && Choose(2)) {
    if (Verbose)
      errs() << "adding a branch, budget = " << Budget << "\n";
    --Budget;
    BranchInst *Br;
    if (0 && Builder->GetInsertBlock()->size() > 0 && Choose(2)) {
      Br = Builder->CreateBr(BBs[0]);
    } else {
      Value *C = genVal(Budget, 1, false, ArgOK);
      Br = Builder->CreateCondBr(C, BBs[0], BBs[0]);
    }
    Branches.push_back(Br);
    BasicBlock *BB = BasicBlock::Create(*C, "", F);
    BBs.push_back(BB);
    Builder->SetInsertPoint(BB);
    return genVal(Budget, Width, ConstOK, ArgOK);
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    if (Verbose)
      errs() << "adding a select with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *L, *R;
    genLR(L, R, Budget, Width);
    Value *C = genVal(Budget, 1, false);
    Value *V = Builder->CreateSelect(C, L, R);
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == 1 && Choose(2)) {
    if (Verbose)
      errs() << "adding an icmp with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *L, *R;
    genLR(L, R, Budget, Width);
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
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    unsigned OldW = Width * 2;
    if (Verbose)
      errs() << "adding a trunc from " << OldW << " to " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, false),
                                    Type::getIntNTy(*C, Width));
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    unsigned OldW = Width / 2;
    if (OldW > 1 && Choose(2))
      OldW = 1;
    if (Verbose)
      errs() << "adding a zext from " << OldW << " to " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *V;
    if (Choose(2))
      V = Builder->CreateZExt(genVal(Budget, OldW, false),
                              Type::getIntNTy(*C, Width));
    else
      V = Builder->CreateSExt(genVal(Budget, OldW, false),
                              Type::getIntNTy(*C, Width));
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    if (Verbose)
      errs() << "adding a binop with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Instruction::BinaryOps Op;
    switch (OneBinop ? 0 : Choose(10)) {
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
    }
    Value *L, *R;
    genLR(L, R, Budget, Width);
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
    Vals.push_back(V);
    return V;
  }

  if (ConstOK && Choose(2)) {
    if (Verbose)
      errs() << "adding a const with width = " << Width
             << " and budget = " << Budget << "\n";
    if (OneConst) {
      return ConstantInt::get(*C, APInt(Width, 1));
    } else {
      int n = Choose((1 << Width) + 1);
      if (n == (1 << Width))
        return UndefValue::get(Type::getIntNTy(*C, Width));
      else
        return ConstantInt::get(*C, APInt(Width, n));
    }
  }

  if (Verbose)
    errs() << "using existing val with width = " << Width
           << " and budget = " << Budget << " and ArgOK = " << ArgOK << "\n";
  std::vector<Value *> Vs;
  for (auto it = Vals.begin(); it != Vals.end(); ++it)
    if ((*it)->getType()->getPrimitiveSizeInBits() == Width)
      Vs.push_back(*it);
  unsigned choices = Vs.size() + ArgOK ? 1 : 0;
  if (choices == 0)
    Done();
  unsigned which = Choose(choices);
  if (which == Vs.size()) {
    Value *V = 0;
    for (auto it = F->arg_begin(); it != F->arg_end(); ++it) {
      if (UsedArgs.find(it) == UsedArgs.end() &&
          it->getType()->getPrimitiveSizeInBits() == Width) {
        UsedArgs.insert(it);
        V = it;
        Vals.push_back(V);
        break;
      }
    }
    check(V);
    return V;
  } else {
    return Vs.at(which);
  }
}

static BasicBlock *chooseTarget(BasicBlock *Avoid = 0) {
  std::vector<inst_iterator> targets;
  auto i = inst_begin(F), ie = inst_end(F);
  ++i;
  for (; i != ie; ++i)
    if (!isa<TerminatorInst>(*i))
      targets.push_back(i);
  auto t = targets[Choose(targets.size())];
  Instruction *I = &*t;
  BasicBlock *BB;
  if (I == I->getParent()->getFirstInsertionPt()) {
    BB = I->getParent();
  } else {
    if (Verbose)
      errs() << "splitting a BB\n";
    BB = I->getParent()->splitBasicBlock(I, "spl");
  }
  return BB;
}

int main(int argc, char **argv) {
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  if (!ForcedChoiceStr.empty()) {
    std::stringstream ss(ForcedChoiceStr);
    copy(std::istream_iterator<int>(ss), std::istream_iterator<int>(),
         std::back_inserter(ForcedChoices));
  }

  if (Seed == INT_MIN) {
    Seed = ::time(0) + ::getpid();
  } else {
    if (All)
      report_fatal_error("can't supply a seed in exhaustive mode");
  }
  ::srand(Seed);

  Shmem =
      (struct shared *)::mmap(0, sizeof(struct shared), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANON, -1, 0);
  check(Shmem != MAP_FAILED);
  Shmem->Children = 0;
  Shmem->NextId = 1;

  Module *M = new Module("", getGlobalContext());
  C = &M->getContext();
  std::vector<Type *> ArgsTy;
  for (int i = 0; i < N + 1; ++i) {
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W));
    ArgsTy.push_back(IntegerType::getIntNTy(*C, 1));
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W / 2));
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W * 2));
  }
  unsigned RetWidth = W;
  auto FuncTy = FunctionType::get(Type::getIntNTy(*C, RetWidth), ArgsTy, 0);
  F = Function::Create(FuncTy, GlobalValue::ExternalLinkage, "func", M);
  BBs.push_back(BasicBlock::Create(*C, "", F));
  Builder = new IRBuilder<true, NoFolder>(BBs[0]);
  int Budget = N;
  Builder->SetInsertPoint(BBs[0]);

  // action happens here
  Value *V = genVal(Budget, RetWidth, false, false);
  // and now every BB has a terminator
  Builder->CreateRet(V);

  // fixup branch targets
  for (auto bi = Branches.begin(), be = Branches.end(); bi != be; ++bi) {
    BasicBlock *BB1 = chooseTarget();
    (*bi)->setSuccessor(0, BB1);
    if ((*bi)->isConditional())
      (*bi)->setSuccessor(1, chooseTarget(BB1));
  }

// finally, fixup the Phis -- first by splitting any BBs where a non-Phi
// precedes a Phi
redo:
  for (auto bb = F->begin(), bbe = F->end(); bb != bbe; ++bb) {
    bool notphi = false;
    for (auto i = bb->begin(), ie = bb->end(); i != ie; ++i) {
      if (!isa<PHINode>(i))
        notphi = true;
      if (notphi && isa<PHINode>(i)) {
        i->getParent()->splitBasicBlock(i, "phisp");
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
      check(Budget == 0);
      Value *V =
          genVal(Budget, P->getType()->getPrimitiveSizeInBits(), true, false);
      P->addIncoming(V, Pred);
    }
  }

  std::string SStr;
  raw_string_ostream SS(SStr);
  legacy::PassManager Passes;
  if (Verify)
    Passes.add(createVerifierPass());
  Passes.add(createPrintModulePass(SS));
  Passes.run(*M);

  if (All) {
    std::stringstream ss;
    ss << "func" << std::to_string(Id);
    ::srand(::time(0) + ::getpid());
    std::string FN = std::to_string(rand() % NumFiles) + ".ll";
    std::string func = SS.str();
    func.replace(func.find("func"), 4, ss.str());
    int fd = open(FN.c_str(), O_RDWR | O_CREAT | O_APPEND, S_IRWXU);
    check(fd > 2);
    /*
     * bad hack -- instead of locking the file we're going to count on an atomic
     * write and abort if it doesn't work -- this works fine on Linux
     */
    unsigned res = write(fd, func.c_str(), func.length());
    check(res == func.length());
    res = close(fd);
    check(res == 0);
  } else {
    outs() << SS.str();
  }

  if (Id == 0) {
    while (Shmem->Children > 0) {
      if (Verbose)
        errs() << "processes = " << Shmem->Children << "\n";
      sleep(1);
    }
  } else {
    --Shmem->Children;
  }
  return 0;
}
