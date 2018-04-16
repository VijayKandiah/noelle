#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"

#include "../include/PDGAnalysis.hpp"

using namespace llvm;

bool llvm::PDGAnalysis::doInitialization (Module &M){
  errs() << "PDGAnalysis at \"doInitialization\"\n" ;
  return false;
}

void llvm::PDGAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.setPreservesAll();
  return ;
}

bool llvm::PDGAnalysis::runOnModule (Module &M){
  errs() << "PDGAnalysis at \"runOnModule\"\n" ;
  this->programDependenceGraph = new PDG();

  this->programDependenceGraph->constructNodes(M);
  constructEdgesFromUseDefs(M);
  constructEdgesFromAliases(M);
  constructEdgesFromControl(M);

  return false;
}

llvm::PDGAnalysis::PDGAnalysis() : ModulePass{ID}{
  return ;
}

llvm::PDGAnalysis::~PDGAnalysis(){
  delete this->programDependenceGraph;
}

llvm::PDG * llvm::PDGAnalysis::getPDG (){
  return this->programDependenceGraph;
}

// Next there is code to register your pass to "opt"
char llvm::PDGAnalysis::ID = 0;
static RegisterPass<PDGAnalysis> X("PDGAnalysis", "Computing the Program Dependence Graph");

// Next there is code to register your pass to "clang"
static PDGAnalysis * _PassMaker = NULL;
static RegisterStandardPasses _RegPass1(PassManagerBuilder::EP_OptimizerLast,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new PDGAnalysis());}}); // ** for -Ox
static RegisterStandardPasses _RegPass2(PassManagerBuilder::EP_EnabledOnOptLevel0,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new PDGAnalysis());}});// ** for -O0

void llvm::PDGAnalysis::constructEdgesFromUseDefs (Module &M){
  for (auto iNodePair : programDependenceGraph->internalNodePairs()) {
    Value *V = iNodePair.first;
    if (V->getNumUses() == 0)
      continue;
    for (auto& U : V->uses()) {
      auto user = U.getUser();
      if (auto userInst = dyn_cast<Instruction>(user))
      {
        auto *edge = programDependenceGraph->createEdgeFromTo(V, user);
        edge->setMemMustRaw(false, true, true);
      }
      else if (auto userArg = dyn_cast<Argument>(user))
      {
        auto *edge = programDependenceGraph->createEdgeFromTo(V, user);
        edge->setMemMustRaw(false, true, true);
      }
    }
  }

  return ;
}

template <class InstI, class InstJ>
void llvm::PDGAnalysis::addEdgeFromMemoryAlias (Function &F, AAResults *aa, InstI *memI, InstJ *memJ, bool storePair){
  DGEdge<Value> *edge;
  switch (aa->alias(MemoryLocation::get(memI),MemoryLocation::get(memJ))) {
    case PartialAlias:
    case MayAlias:
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)memJ);
      //if (F.getName() == "main") edge->print(errs() << "May:\t") << "\n";
      edge->setMemMustRaw(true, false, !storePair);
      break;
    case MustAlias:
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)memJ);
      //if (F.getName() == "main") edge->print(errs() << "Must:\t") << "\n";
      edge->setMemMustRaw(true, true, !storePair);
      break;
  }
}

void llvm::PDGAnalysis::addEdgeFromFunctionModRef (Function &F, AAResults *aa, StoreInst *memI, CallInst *call){
  DGEdge<Value> *edge;
  switch (aa->getModRefInfo(call, MemoryLocation::get(memI))) {
    case MRI_Ref:
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)call);
      edge->setMemMustRaw(true, false, true);
      //if (F.getName() == "main") edge->print(errs() << "Ref:\t") << "\n";
      break;
    case MRI_Mod:
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)call);
      edge->setMemMustRaw(true, false, false);
      //if (F.getName() == "main") edge->print(errs() << "Mod:\t") << "\n";
      break;
    case MRI_ModRef:
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)call);
      edge->setMemMustRaw(true, false, true);
      //if (F.getName() == "main") edge->print(errs() << "ModRef:\t") << "\n";
      edge = programDependenceGraph->createEdgeFromTo((Value*)memI, (Value*)call);
      edge->setMemMustRaw(true, false, false);
      break;
  }
}

void llvm::PDGAnalysis::addEdgeFromFunctionModRef (Function &F, AAResults *aa, LoadInst *memI, CallInst *call){
  DGEdge<Value> *edge;
  switch (aa->getModRefInfo(call, MemoryLocation::get(memI))) {
    case MRI_Ref:
      break;
    case MRI_Mod:
    case MRI_ModRef:
      edge = programDependenceGraph->createEdgeFromTo((Value*)call, (Value*)memI);
      //if (F.getName() == "main") edge->print(errs() << "ModRef:\t") << "\n";
      edge->setMemMustRaw(true, false, true);
      break;
  }
}

void llvm::PDGAnalysis::iterateInstForStoreAliases(Function &F, AAResults *aa, StoreInst *J) {
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *store = dyn_cast<StoreInst>(&I)) {
        if (store != J)
          addEdgeFromMemoryAlias<StoreInst, StoreInst>(F, aa, store, J, true);
      } else if (auto *load = dyn_cast<LoadInst>(&I)) {
        addEdgeFromMemoryAlias<LoadInst, StoreInst>(F, aa, load, J, false);
      }
    }
  }
}

void llvm::PDGAnalysis::iterateInstForLoadAliases(Function &F, AAResults *aa, LoadInst *J) {
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *store = dyn_cast<StoreInst>(&I)) {
        addEdgeFromMemoryAlias<StoreInst, LoadInst>(F, aa, store, J, false);
      }
    }
  }
}

void llvm::PDGAnalysis::iterateInstForModRef(Function &F, AAResults *aa, CallInst &call) {
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *load = dyn_cast<LoadInst>(&I)) {
        addEdgeFromFunctionModRef(F, aa, load, &call);
      } else if (auto *store = dyn_cast<StoreInst>(&I)) {
        addEdgeFromFunctionModRef(F, aa, store, &call);
      }
    }
  }
}

void llvm::PDGAnalysis::constructEdgesFromAliases (Module &M){
  /*
   * Use alias analysis on stores, loads, and function calls to construct PDG edges
   */
  for (auto &F : M) {
    if (F.empty()) continue ;
    auto aaResults = &(getAnalysis<AAResultsWrapperPass>(F).getAAResults());
    for (auto &B : F) {
      for (auto &I : B) {
        if (auto* store = dyn_cast<StoreInst>(&I)) {
          iterateInstForStoreAliases(F, aaResults, store);
        } else if (auto *load = dyn_cast<LoadInst>(&I)) {
          iterateInstForLoadAliases(F, aaResults, load);
        } else if (auto *call = dyn_cast<CallInst>(&I)) {
          iterateInstForModRef(F, aaResults, *call);
        }
      }
    }
  }
}

void llvm::PDGAnalysis::constructEdgesFromControl (Module &M){
  for (auto &F : M) {
    if (F.empty()) continue ;
    auto postDomTree = &getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
    this->programDependenceGraph->constructControlEdgesForFunction(F, *postDomTree);
  }
}