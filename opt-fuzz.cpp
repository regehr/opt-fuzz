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
#include <algorithm>
#include <fcntl.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

using namespace llvm;

static cl::opt<int> Cores("cores", cl::desc("How many cores to use (default=1)"),
                          cl::init(1));
static cl::opt<int> W("width", cl::desc("Base integer width (default=2)"),
                      cl::init(2));
static cl::opt<int>
    N("num-insns", cl::desc("Number of instructions (default=2)"), cl::init(2));
static cl::opt<bool> Branch("branches",
                            cl::desc("Generate branches (default=false) (broken don't use)"),
                            cl::init(false));
static cl::opt<int> NumFiles("num-files",
                             cl::desc("Number of output files (default=1000)"),
                             cl::init(1000));
static cl::opt<bool>
    OneICmp("oneicmp", cl::desc("Only emit one kind of icmp (default=false)"),
            cl::init(false));
static cl::opt<bool>
    OneBinop("onebinop",
             cl::desc("Only emit one kind of binop (default=false)"),
             cl::init(false));
static cl::opt<bool>
    NoUB("noub", cl::desc("Do not put UB flags on binops (default=false)"),
         cl::init(false));
static cl::opt<bool>
    Geni1("geni1", cl::desc("Functions return i1 instead of iN (default=false)"),
         cl::init(false));
static cl::opt<bool>
    FewConsts("fewconsts",
             cl::desc("Instead of trying all values of every constant, try a few selected constants (default=false)"),
             cl::init(false));
static cl::opt<bool>
    Fuzz("fuzz", cl::desc("Generate one program instead of all of them"),
         cl::init(false));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<int> Seed("seed", cl::desc("PRNG seed"), cl::init(INT_MIN));
static cl::opt<std::string> ForcedChoiceStr("choices",
                                            cl::desc("Force these choices"));
static cl::opt<bool> Verify("verify", cl::desc("Run the LLVM verifier"),
                            cl::init(true));

static std::vector<int> ForcedChoices;

#define MAX 100

struct shared {
  std::atomic_long NextId;
  pthread_mutex_t Lock;
  pthread_mutexattr_t LockAttr;
  pthread_cond_t Cond[MAX];
  int Waiting[MAX];
  pthread_condattr_t CondAttr;
  int Running;
  bool Stop;
} * Shmem;
static std::string Choices;
static long Id;

static int Depth = 1;
static bool Init = false;

static void die(const char *str) {
  errs() << str << "ABORTING: \n";
  // not checking return value here...
  if (Init)
    pthread_mutex_lock(&Shmem->Lock);
  Shmem->Stop = 1;
  if (Init) {
    pthread_cond_broadcast(Shmem->Cond);
    pthread_mutex_unlock(&Shmem->Lock);
  }
  exit(-1);
}

static void decrease_runners(void) {
  if (pthread_mutex_lock(&Shmem->Lock) != 0)
    die("lock failed");

  assert(Shmem->Running <= Cores);

  Shmem->Running--;
  // FIXME could cache the max depth, perhaps don't care
  for (int i=MAX-1; i>=0; --i) {
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

static void increase_runners(int Depth) {
  if (pthread_mutex_lock(&Shmem->Lock) != 0)
    die("lock failed");

  if (Depth >= MAX)
    die("oops, you'll need to rebuild opt-fuzz with a larger MAX");
  assert(Shmem->Running <= Cores);

  while (Shmem->Running >= Cores) {
    Shmem->Waiting[Depth]++;
    if (Shmem->Stop)
      exit(-1);
    if (pthread_cond_wait(&Shmem->Cond[Depth], &Shmem->Lock))
      die("pthread_cond_wait failed");
    if (Shmem->Stop)
      exit(-1);
  }
  Shmem->Running++;

  if (pthread_mutex_unlock(&Shmem->Lock) != 0)
    die("unlock failed");
}

static unsigned Choose(unsigned n) {
  assert(n > 0);
  if (!Fuzz) {
    for (unsigned i = 0; i < (n - 1); ++i) {
      if (Shmem->Stop)
        exit(-1);
      int ret = ::fork();
      if(ret == -1)
        die("fork failed");
      if (ret == 0) {
        // child
        Id = Shmem->NextId.fetch_add(1);
        Choices += std::to_string(i) + " ";
        ++Depth;
        return i;
      }
      // parent
      increase_runners(Depth);
      waitpid(-1, 0, WNOHANG);
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

static IRBuilder<NoFolder> *Builder;
static LLVMContext C;
static std::vector<Value *> Vals;
static Function *F;
static std::set<Argument *> UsedArgs;
static std::vector<BasicBlock *> BBs;
static std::vector<BranchInst *> Branches;

static Value *genVal(int &Budget, unsigned Width, bool ConstOK,
                     bool ArgOK = true);

static void genLR(Value *&L, Value *&R, int &Budget, unsigned Width) {
  L = genVal(Budget, Width, true);
  R = genVal(Budget, Width, !isa<Constant>(L) && !isa<UndefValue>(L));
}

static Value *genVal(int &Budget, unsigned Width, bool ConstOK, bool ArgOK) {
  if (Branch && Budget > 0 && Choose(2)) {
    if (Verbose)
      errs() << "adding a phi, budget = " << Budget << "\n";
    --Budget;
    Value *V = Builder->CreatePHI(Type::getIntNTy(C, Width), N);
    Vals.push_back(V);
    assert(V);
    return V;
  }

  if (Branch && Budget > 0 && Budget != N && Choose(2)) {
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
    BasicBlock *BB = BasicBlock::Create(C, "", F);
    BBs.push_back(BB);
    Builder->SetInsertPoint(BB);
    auto V = genVal(Budget, Width, ConstOK, ArgOK);
    assert(V);
    return V;
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
    assert(V);
    return V;
  }

  if (Budget > 0 && Width == 1 && Choose(2)) {
    if (Verbose)
      errs() << "adding an icmp with width = " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *L, *R;
    genLR(L, R, Budget, W);
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
    assert(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    unsigned OldW = Width * 2;
    if (Verbose)
      errs() << "adding a trunc from " << OldW << " to " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, false),
                                    Type::getIntNTy(C, Width));
    Vals.push_back(V);
    assert(V);
    return V;
  }

  if (Budget > 0 && Width == 1 && Choose(2)) {
    unsigned OldW = W;
    if (Verbose)
      errs() << "adding a trunc from " << OldW << " to " << Width
             << " and budget = " << Budget << "\n";
    --Budget;
    Value *V = Builder->CreateTrunc(genVal(Budget, OldW, false),
                                    Type::getIntNTy(C, 1));
    Vals.push_back(V);
    assert(V);
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
                              Type::getIntNTy(C, Width));
    else
      V = Builder->CreateSExt(genVal(Budget, OldW, false),
                              Type::getIntNTy(C, Width));
    Vals.push_back(V);
    return V;
  }

  if (Budget > 0 && Width == W && Choose(2)) {
    if (Verbose)
      errs() << "adding a binop with width = " << Width
             << " and budget = " << Budget << "\n";
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
    assert(V);
    return V;
  }

  /*
   * from this point on we're not generating instructions and hence
   * not consuming budget
   */

  if (ConstOK && Choose(2)) {
    if (Verbose)
      errs() << "adding a const with width = " << Width
             << " and budget = " << Budget << "\n";
    if (FewConsts) {
      int n = Choose(7);
      switch (n) {
      case 0:
        return UndefValue::get(Type::getIntNTy(C, Width));
      case 1:
        return ConstantInt::get(C, APInt(Width, 0));
      case 2:
        return ConstantInt::get(C, APInt(Width, 1));
      case 3:
        return ConstantInt::get(C, APInt(Width, -1));
      case 4:
        return ConstantInt::get(C, APInt(Width, Shmem->NextId % (Width + 3)));
      case 5:
        return ConstantInt::get(C, APInt::getSignedMaxValue(Width));
      case 6:
        return ConstantInt::get(C, APInt::getSignedMinValue(Width));
      default:
        assert(false);
      }
      return ConstantInt::get(C, APInt(Width, 1));
    } else {
      int n = Choose((1 << Width) + 1);
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
    if (Verbose)
      errs() << "using function argument with width = " << Width << "\n";
    std::vector<Value *> Vs;
    bool found = false;
    for (auto &it : F->args()) {
      Argument *a = &it;
      if (a->getType()->getPrimitiveSizeInBits() != Width)
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
      errs() << "Error: ran out of function arguments of width " << Width << "\n";
    return Vs.at(Choose(Vs.size()));
  }

  if (Verbose)
    errs() << "using existing val with width = " << Width << "\n";
  std::vector<Value *> Vs;
  for (auto &it : Vals)
    if (it->getType()->getPrimitiveSizeInBits() == Width)
      Vs.push_back(it);
  // this can happen when no values have been created yet, no big deal
  if (Vs.size() == 0)
    exit(0);
  return Vs.at(Choose(Vs.size()));
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
  if (I == &*I->getParent()->getFirstInsertionPt()) {
    BB = I->getParent();
  } else {
    if (Verbose)
      errs() << "splitting a BB\n";
    BB = I->getParent()->splitBasicBlock(I, "spl");
  }
  return BB;
}

static void generate(Module *&M) {
  M = new Module("", C);
  std::vector<Type *> ArgsTy;
  for (int i = 0; i < N + 2; ++i) {
    ArgsTy.push_back(IntegerType::getIntNTy(C, W));
    ArgsTy.push_back(IntegerType::getIntNTy(C, 1));
    ArgsTy.push_back(IntegerType::getIntNTy(C, W / 2));
    ArgsTy.push_back(IntegerType::getIntNTy(C, W * 2));
  }
  auto FuncTy = FunctionType::get(Type::getIntNTy(C, Geni1 ? 1 : W), ArgsTy, 0);
  F = Function::Create(FuncTy, GlobalValue::ExternalLinkage, "func", M);
  BBs.push_back(BasicBlock::Create(C, "", F));
  Builder = new IRBuilder<NoFolder>(BBs[0]);
  int Budget = N;
  Builder->SetInsertPoint(BBs[0]);

  // the magic happens in genVal()
  Value *V;
  if (Geni1)
    V = genVal(Budget, 1, false, false);
  else
    V = genVal(Budget, W, false, false);
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

void output(Module *M) {
  std::string SStr;
  raw_string_ostream SS(SStr);
  legacy::PassManager Passes;
  if (Verify)
    Passes.add(createVerifierPass());
  Passes.add(createPrintModulePass(SS));
  Passes.run(*M);

  if (!Fuzz) {
    std::stringstream ss;
    ss << "func" << std::to_string(Id);
    ::srand(::time(0) + ::getpid());
    std::string FN = std::to_string(rand() % NumFiles) + ".ll";
    std::string func = SS.str();
    func.replace(func.find("func"), 4, ss.str());
    int fd = open(FN.c_str(), O_RDWR | O_CREAT | O_APPEND, S_IRWXU);
    if (fd < 2)
      die("open failed");
    /*
     * bad hack -- instead of locking the file we're going to count on an atomic
     * write and bail if it doesn't work -- this works fine on Linux
     */
    unsigned res = write(fd, func.c_str(), func.length());
    if (res != func.length())
      die("non-atomic write");
    res = close(fd);
    assert(res == 0);
  } else {
    outs() << SS.str();
  }
}

int main(int argc, char **argv) {
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "llvm codegen stress-tester\n");

  if (W < 2)
    die("Width must be >= 2");

  if (!ForcedChoiceStr.empty()) {
    std::stringstream ss(ForcedChoiceStr);
    copy(std::istream_iterator<int>(ss), std::istream_iterator<int>(),
         std::back_inserter(ForcedChoices));
  }

  if (Seed == INT_MIN) {
    Seed = ::time(0) + ::getpid();
  } else {
    if (!Fuzz)
      report_fatal_error("can't supply a seed in exhaustive mode");
  }
  ::srand(Seed);

  Shmem =
      (struct shared *)::mmap(0, sizeof(struct shared), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANON, -1, 0);
  if (Shmem == MAP_FAILED)
    die("mmap failed");
  Shmem->NextId = 1;
  Shmem->Running = 1;
  if (pthread_mutexattr_init(&Shmem->LockAttr) != 0)
    die("pthread_mutexattr_init failed");
  if (pthread_mutexattr_setpshared(&Shmem->LockAttr, PTHREAD_PROCESS_SHARED) != 0)
    die("pthread_mutexattr_setpshared failed");
  if (pthread_mutex_init(&Shmem->Lock, &Shmem->LockAttr) != 0)
    die("pthread_mutex_init failed");
  if (pthread_condattr_init(&Shmem->CondAttr) != 0)
    die("pthread_condattr_init failed");
  if (pthread_condattr_setpshared(&Shmem->CondAttr, PTHREAD_PROCESS_SHARED) != 0)
    die("pthread_condattr_setpshared failed");
  for (int i=0; i<MAX; ++i) {
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

  Module *M;
  generate(M);
  output(M);

  if (::getpid() == original_pid) {
    char buf[1];
    ::close(p[1]);
    ::read(p[0], buf, 1);
    for (int i=0; i<MAX; i++) {
      if (Shmem->Waiting[i] != 0)
        errs() << "oops, there are waiting processes at " << i << "\n";
    }
  }

  return 0;
}
