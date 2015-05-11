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
using namespace llvm;

static const int Cpus = 4;

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static cl::opt<bool> All("all", cl::desc("Generate all programs"),
                         cl::init(false));

static std::vector<int> Choices;

struct shared {
  std::atomic_long Processes;
  std::atomic_long NextId;
} * Shmem;

static long Id;

static int Choose(int n) {
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
static Function::arg_iterator NextArg;

static Value *genVal(int &Budget, int Width, bool ConstOK = true) {
  if (Budget > 0 && Choose(2)) {
    // make a new instruction
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
    Value *L = genVal(Budget, Width);
    bool Lconst = dyn_cast<ConstantInt>(L);
    Value *R = genVal(Budget, Width, !Lconst);
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
    int n = Choose((1 << Width) + 1);
    if (n == (1 << Width))
      return UndefValue::get(Type::getIntNTy(*C, Width));
    else
      return ConstantInt::get(*C, APInt(Width, n));
  }

  assert(NextArg);
  Vals.push_back(NextArg);
  ++NextArg;
  return Vals.at(Choose(Vals.size()));
}

static const int W = 2; // width
static const int N = 2; // number of instructions to generate

int main(int argc, char **argv) {
  srand(::time(0) + ::getpid());
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  Shmem =
      (struct shared *)mmap(NULL, sizeof(struct shared), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(Shmem != MAP_FAILED);
  Shmem->Processes = 1;
  Shmem->NextId = 1;

  if (OutputFilename.empty()) {
    OutputFilename = "-";
  } else {
    if (All)
      report_fatal_error("cannot specify output file in exhaustive mode");
  }

  Module *M = new Module("/tmp/autogen.bc", getGlobalContext());
  C = &M->getContext();
  static std::vector<Type *> ArgsTy;
  for (int i = 0; i < N + 1; ++i)
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W));
  FunctionType *FuncTy = FunctionType::get(Type::getIntNTy(*C, W), ArgsTy, 0);
  Function *F =
      Function::Create(FuncTy, GlobalValue::ExternalLinkage, "autogen", M);
  NextArg = F->arg_begin();
  Builder = new IRBuilder<true, NoFolder>(BasicBlock::Create(*C, "", F));

  int Budget = N;
  Value *V = genVal(Budget, W);
  Builder->CreateRet(V);

  std::string ChoiceStr = "";
  for (std::vector<int>::iterator it = Choices.begin(); it != Choices.end();
       ++it)
    ChoiceStr += std::to_string(*it) + " ";
  ChoiceStr.erase(ChoiceStr.end() - 1);

  if (All) {
    std::stringstream ss;
    ss.width(7);
    ss.fill('0');
    ss << Id << ".ll";
    OutputFilename = ss.str();
  }

  std::unique_ptr<tool_output_file> Out;
  std::error_code EC;
  Out.reset(new tool_output_file(OutputFilename, EC, sys::fs::F_None));
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  Out->os() << "; Choices: " << ChoiceStr << "\n";
  legacy::PassManager Passes;
  Passes.add(createVerifierPass());
  Passes.add(createPrintModulePass(Out->os()));
  Passes.run(*M);
  Out->keep();

  while (::waitpid(-1, 0, 0)) {
    if (errno == ECHILD)
      break;
  }
  --Shmem->Processes;
  return 0;
}
