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

#include <unistd.h>
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
using namespace llvm;

static const int W = 3; // width
static const int N = 2; // number of instructions to generate
static const int MaxArgs = N + 2;

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static cl::opt<bool> All("all", cl::desc("Generate all programs"),
                         cl::init(false));

static std::vector<int> Choices;

static int Choose(int n) {
  if (All) {
    for (int i = 0; i < n; ++i) {
      int ret = ::fork();
      assert(ret != -1);
      if (ret == 0) {
        Choices.push_back(i);
        return i;
      }
    }
    exit(0);
  } else {
    int i = rand() % n;
    Choices.push_back(i);
    return i;
  }
}

static std::vector<Type *> ArgsTy;
static int budget = N;
static IRBuilder<true, NoFolder> *builder;
static LLVMContext *C;
static std::vector<Value *> Vals;
static Function::arg_iterator NextArg;

static void freshArg() {
  assert(NextArg);
  Vals.push_back(NextArg);
  ++NextArg;
}

static Value *getVal() {
  switch (Choose((budget > 0) ? 3 : 2)) {
  case 0:
    // return a value that was already available
    freshArg();
    return Vals.at(Choose(Vals.size()));
  case 1:
    // return a constant
    return ConstantInt::get(*C, APInt(W, Choose(1 << W)));
  case 2: {
    // make a new instruction
    --budget;
    Value *V;
    switch (Choose(8)) {
    case 0:
      V = builder->CreateAdd(getVal(), getVal());
      break;
    case 1:
      V = builder->CreateSub(getVal(), getVal());
      break;
    case 2:
      V = builder->CreateMul(getVal(), getVal());
      break;
    case 3:
      V = builder->CreateSDiv(getVal(), getVal());
      break;
    case 4:
      V = builder->CreateUDiv(getVal(), getVal());
      break;
    case 5:
      V = builder->CreateAnd(getVal(), getVal());
      break;
    case 6:
      V = builder->CreateOr(getVal(), getVal());
      break;
    case 7:
      V = builder->CreateXor(getVal(), getVal());
      break;
    }
    Vals.push_back(V);
    return V;
  }
  default:
    assert(0);
  }
}

int main(int argc, char **argv) {
  srand(::time(0) + ::getpid());
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  Module *M = new Module("/tmp/autogen.bc", getGlobalContext());
  C = &M->getContext();
  for (int i = 0; i < MaxArgs; ++i)
    ArgsTy.push_back(IntegerType::getIntNTy(*C, W));
  FunctionType *FuncTy = FunctionType::get(Type::getIntNTy(*C, W), ArgsTy, 0);
  Function *F =
      Function::Create(FuncTy, GlobalValue::ExternalLinkage, "autogen", M);
  NextArg = F->arg_begin();
  builder = new IRBuilder<true, NoFolder>(BasicBlock::Create(*C, "", F));

  Value *V = getVal();
  builder->CreateRet(V);

  if (OutputFilename.empty())
    OutputFilename = "-";

  std::string ChoiceStr = "";
  for (std::vector<int>::iterator it = Choices.begin(); it != Choices.end();
       ++it)
    ChoiceStr += std::to_string(*it) + "_";
  ChoiceStr.erase(ChoiceStr.end()-1);

  if (All)
    OutputFilename = ChoiceStr + ".ll";

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

  return 0;
}
