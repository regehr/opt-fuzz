//===-- llvm-stress.cpp - Generate random LL files to stress-test LLVM ----===//
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

static const int W = 8; // width
static const int N = 1; // number of instructions to generate

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static std::vector<Type *> ArgsTy;
static int budget = N;
IRBuilder<true, llvm::NoFolder> *builder;
LLVMContext *C;
std::set<Value *> Vals;
Function *F;
Module *M;

Value *getVal() {

  if (budget > 0) {
    --budget;
    Value *V = builder->CreateAdd(getVal(), getVal());
    Vals.insert(V);
    return V;
  }

  return ConstantInt::get(*C, APInt(W, 2));
}

int main(int argc, char **argv) {
  srand(::time(0) + ::getpid());
  llvm::PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  std::unique_ptr<Module> Mod(new Module("/tmp/autogen.bc", getGlobalContext()));
  M = Mod.get();
  C = &M->getContext();
  FunctionType *FuncTy = FunctionType::get(Type::getIntNTy(*C, W), ArgsTy, 0);
  F = Function::Create(FuncTy, GlobalValue::ExternalLinkage, "autogen", M);
  builder = new IRBuilder<true, llvm::NoFolder>(BasicBlock::Create(*C, "", F));

  Value *V = getVal();
  builder->CreateRet(V);

  if (OutputFilename.empty())
    OutputFilename = "-";
  std::unique_ptr<tool_output_file> Out;
  std::error_code EC;
  Out.reset(new tool_output_file(OutputFilename, EC, sys::fs::F_None));
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  legacy::PassManager Passes;
  Passes.add(createVerifierPass());
  Passes.add(createPrintModulePass(Out->os()));
  Passes.run(*M);
  Out->keep();

  return 0;
}
