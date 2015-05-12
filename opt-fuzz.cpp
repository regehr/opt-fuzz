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
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassNameParser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
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

static const unsigned W = 2; // width
static const int N = 3;      // number of instructions to generate
static const int FileDigits = 3;

static const int Cpus = 5;

static cl::opt<bool> All("all", cl::desc("Generate all programs"),
                         cl::init(false));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<int> Seed("seed", cl::desc("PRNG seed"), cl::init(INT_MIN));

struct shared {
  std::atomic_long Processes;
  std::atomic_long NextId;
} * Shmem;
static std::vector<int> Choices;
static long Id;

static int Choose(int n) {
  assert(n > 0);
  if (All) {
    for (int i = 0; i < (n - 1); ++i) {
      int ret = ::fork();
      assert(ret != -1);
      Id = Shmem->NextId.fetch_add(1);
      if (ret == 0) {
        ++Shmem->Processes;
        Choices.push_back(i);
        return i;
      } else {
        if (Shmem->Processes > Cpus) {
          ret = ::wait(0);
          assert(ret != -1);
        }
      }
    }
    Choices.push_back(n - 1);
    return n - 1;
  } else {
    int i = rand() % n;
    Choices.push_back(i);
    return i;
  }
}

static IRBuilder<true, NoFolder> *Builder;
static LLVMContext *C;
static std::vector<Value *> Vals;
static Function *F;
static std::set<Argument *> UsedArgs;

static Value *genVal(int &Budget, unsigned Width, bool ConstOK = true);

static void genLR(Value *&L, Value *&R, int &Budget, unsigned Width) {
  L = genVal(Budget, Width);
  bool Lconst = isa<Constant>(L) || isa<UndefValue>(L);
  R = genVal(Budget, Width, /* ConstOK = */ !Lconst);
}

static Value *genVal(int &Budget, unsigned Width, bool ConstOK) {
  if (Budget > 0 && Choose(2)) {
    if (Verbose)
      errs() << "adding a select with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *L, *R;
    genLR(L, R, Budget, Width);
    Value *C = genVal(Budget, 1, /* ConstOK = */ false);
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
    switch (Choose(10)) {
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
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, /* ConstOK = */ false),
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
      V = Builder->CreateZExt(genVal(Budget, OldW, /* ConstOK = */ false),
                              Type::getIntNTy(*C, Width));
    else
      V = Builder->CreateSExt(genVal(Budget, OldW, /* ConstOK = */ false),
                              Type::getIntNTy(*C, Width));
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Choose(2)) {
    if (Verbose)
      errs() << "adding a binop with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Instruction::BinaryOps Op;
    switch (Choose(10)) {
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
    Vals.push_back(V);
    return V;
  }

  if (ConstOK && Choose(2)) {
    if (Verbose)
      errs() << "adding a const with width = " << Width
             << " and budget = " << Budget << "\n";
    int n = Choose((1 << Width) + 1);
    if (n == (1 << Width))
      return UndefValue::get(Type::getIntNTy(*C, Width));
    else
      return ConstantInt::get(*C, APInt(Width, n));
  }

  if (Verbose)
    errs() << "adding an arg with width = " << Width
           << " and budget = " << Budget << "\n";
  bool found = false;
  for (auto it = F->arg_begin(); it != F->arg_end(); ++it) {
    if (UsedArgs.find(it) == UsedArgs.end() &&
        it->getType()->getPrimitiveSizeInBits() == Width) {
      UsedArgs.insert(it);
      Vals.push_back(it);
      found = true;
      break;
    }
  }
  assert(found);
  std::vector<Value *> Vs;
  for (auto it = Vals.begin(); it != Vals.end(); ++it)
    if ((*it)->getType()->getPrimitiveSizeInBits() == Width)
      Vs.push_back(*it);
  assert(Vs.size() > 0);
  return Vs.at(Choose(Vs.size()));
}

int main(int argc, char **argv) {
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  if (Seed == INT_MIN) {
    Seed = ::time(0) + ::getpid();
  } else {
    if (All)
      report_fatal_error("can't supply a seed in exhaustive mode");
  }
  srand(Seed);

  Shmem =
      (struct shared *)mmap(NULL, sizeof(struct shared), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANON, -1, 0);
  assert(Shmem != MAP_FAILED);
  Shmem->Processes = 1;
  Shmem->NextId = 1;

  Module *M = new Module("opt-fuzz", getGlobalContext());
  C = &M->getContext();
  std::vector<Type *> ArgsTy;
  for (int i = 0; i < N + 1; ++i) {
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W));
    ArgsTy.push_back(IntegerType::getIntNTy(*C, 1));
    if (W / 2 != 1)
      ArgsTy.push_back(IntegerType::getIntNTy(*C, W / 2));
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W * 2));
  }
  unsigned RetWidth = W;
  auto FuncTy = FunctionType::get(Type::getIntNTy(*C, RetWidth), ArgsTy, 0);
  F = Function::Create(FuncTy, GlobalValue::ExternalLinkage, "func", M);
  Builder = new IRBuilder<true, NoFolder>(BasicBlock::Create(*C, "", F));

  int Budget = N;
  Value *V = genVal(Budget, RetWidth);
  Builder->CreateRet(V);

  std::string ChoiceStr = "";
  for (auto it = Choices.begin(); it != Choices.end(); ++it)
    ChoiceStr += std::to_string(*it) + " ";
  ChoiceStr.erase(ChoiceStr.end() - 1);

  std::string SStr;
  raw_string_ostream SS(SStr);

  legacy::PassManager Passes;
  Passes.add(createVerifierPass());
  Passes.add(createPrintModulePass(SS));
  Passes.run(*M);

  if (All) {
    std::stringstream ss;
    std::stringstream ss2;
    ss.width(7);
    ss.fill('0');
    ss << Id;
    ss2 << "func" << Id;
    std::string s = ss.str();
    int pos = s.length() - FileDigits;
    std::string FN = s.substr(pos, FileDigits) + ".ll";
    s.erase(pos, FileDigits);
    std::string func = SS.str();
    func.replace(func.find("func"), 4, ss2.str());
    int fd = open(FN.c_str(), O_RDWR | O_CREAT | O_APPEND, S_IRWXU);
    assert(fd > 2);
    /*
     * bad hack -- instead of locking the file we're going to count on an atomic
     * write and abort if it doesn't work -- this works fine on Linux
     */
    unsigned res = write(fd, func.c_str(), func.length());
    assert(res == func.length());
    res = close(fd);
    assert(res == 0);
  } else {
    outs() << "; seed = " << Seed << "\n";
    outs() << SS.str();
  }

  if (All) {
    while (::waitpid(-1, 0, 0)) {
      if (errno == ECHILD)
        break;
    }
  }
  --Shmem->Processes;
  return 0;
}
