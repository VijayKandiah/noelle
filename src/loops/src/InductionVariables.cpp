/*
 * Copyright 2016 - 2019  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "InductionVariables.hpp"

using namespace llvm;

InductionVariables::InductionVariables (LoopsSummary &LIS, ScalarEvolution &SE, SCCDAG &sccdag)
  : loopToIVsMap{}, loopToGoverningIVMap{} {
  for (auto &loop : LIS.loops) {
    loopToIVsMap[loop.get()] = std::set<InductionVariable *>();

    /*
     * Fetch the loop header.
     */
    auto header = loop->getHeader();

    /*
     * Iterate over all phis within the loop header.
     */
    for (auto &phi : header->phis()) {
      phi.print(errs() << "Checking PHI: "); errs() << "\n";
      auto scev = SE.getSCEV(&phi);
      if (!scev || scev->getSCEVType() != SCEVTypes::scAddRecExpr) continue;
      errs() << "IS IV\n";

      auto sccContainingIV = sccdag.sccOfValue(&phi);
      auto IV = new InductionVariable(loop.get(), l, SE, &phi, *sccContainingIV); 
      loopToIVsMap[loop.get()].insert(IV);
      errs() << "BUILT IV\n";

      auto exitBlocks = LIS.getLoopNestingTreeRoot()->getLoopExitBasicBlocks();
      LoopGoverningIVAttribution attribution(*IV, *sccContainingIV, exitBlocks);
      if (attribution.isSCCContainingIVWellFormed()) {
        loopToGoverningIVMap[loop.get()] = IV;
      }
    }
  }
}

InductionVariables::~InductionVariables () {
  for (auto loopIVs : loopToIVsMap) {
    for (auto IV : loopIVs.second) {
      delete IV;
    }
  }
  loopToIVsMap.clear();
  loopToGoverningIVMap.clear();
}

std::set<InductionVariable *> &InductionVariables::getInductionVariables(LoopSummary &LS) {
  return loopToIVsMap.at(&LS);
}

InductionVariable *InductionVariables::getLoopGoverningInductionVariable (LoopSummary &LS) {
  if (loopToGoverningIVMap.find(&LS) == loopToGoverningIVMap.end()) return nullptr;
  return loopToGoverningIVMap.at(&LS);
}

InductionVariable::InductionVariable  (LoopSummary *LS, Loop *llvmLoop, ScalarEvolution &SE, PHINode *headerPHI, SCC &scc)
  : scc{scc}, headerPHI{headerPHI}, startValue{nullptr},
    stepSize{nullptr}, compositeStepSize{nullptr}, expansionOfCompositeStepSize{} {

  /*
   * Collect intermediate values of the IV within the loop (by traversing its strongly connected component)
   */
  std::queue<DGNode<Value> *> ivIntermediateValues;
  std::set<Value *> valuesVisited;
  ivIntermediateValues.push(scc.fetchNode(headerPHI));
  while (!ivIntermediateValues.empty()) {
    auto node = ivIntermediateValues.front();
    auto value = node->getT();
    ivIntermediateValues.pop();

    if (valuesVisited.find(value) != valuesVisited.end()) continue;
    valuesVisited.insert(value);

    if (auto phi = dyn_cast<PHINode>(value)) {
      this->PHIs.insert(phi);
      this->allInstructions.insert(cast<Instruction>(phi));
    } else if (auto I = dyn_cast<Instruction>(value)) {
      this->accumulators.insert(I);
      this->allInstructions.insert(I);
    }

    for (auto edge : node->getIncomingEdges()) {
      if (!edge->isDataDependence()) continue;
      if (!scc.isInternal(edge->getOutgoingT())) continue;
      ivIntermediateValues.push(edge->getOutgoingNode());
    }
  }

  /*
   * Fetch initial value of induction variable
   */
  auto bbs = LS->getBasicBlocks();
  for (auto i = 0; i < headerPHI->getNumIncomingValues(); ++i) {
    auto incomingBB = headerPHI->getIncomingBlock(i);
    if (bbs.find(incomingBB) == bbs.end()) {
      this->startValue = headerPHI->getIncomingValue(i);
      break;
    }
  }

  auto headerSCEV = SE.getSCEV(headerPHI);
  assert(headerSCEV->getSCEVType() == SCEVTypes::scAddRecExpr);
  auto stepSCEV = cast<SCEVAddRecExpr>(headerSCEV)->getStepRecurrence(SE);

  auto M = headerPHI->getFunction()->getParent();
  DataLayout DL(M);
  const char name = 'a';
  auto expander = new SCEVExpander(SE, DL, &name);
  switch (stepSCEV->getSCEVType()) {
    case SCEVTypes::scConstant:
      this->stepSize = cast<SCEVConstant>(stepSCEV)->getValue();
      stepSize->print(errs() << "Constant step: "); errs() << "\n";
      break;
    case SCEVTypes::scUnknown:
      this->stepSize = cast<SCEVUnknown>(stepSCEV)->getValue();
      stepSize->print(errs() << "Arbitrary value step: "); errs() << "\n";
      break;
    default:
      errs() << "Custom\n";
      this->stepSize = expander->getExactExistingExpansion(stepSCEV, headerPHI, llvmLoop);
      if (!this->stepSize) {
        auto &entryBlock = headerPHI->getParent()->getParent()->getEntryBlock();
        expander->setInsertPoint(entryBlock.getTerminator());
        auto endCompositeValue = expander->expandCodeFor(stepSCEV);
        expander->clearInsertPoint();
        assert(isa<Instruction>(endCompositeValue));
        auto currValue = entryBlock.getTerminator();
        auto endValue = cast<Instruction>(endCompositeValue)->getNextNode();
        while (currValue != endValue) {
          expansionOfCompositeStepSize.push_back(currValue);
          auto prevValue = currValue;
          currValue = currValue->getNextNode();
          prevValue->removeFromParent();
        }
      }
      break;
  }

  delete expander;
}

InductionVariable::~InductionVariable () {
  for (auto expandedInst : expansionOfCompositeStepSize) {
    expandedInst->deleteValue();
  }
}

void InductionVariable::determineStepSize(ScalarEvolution &SE, LoopSummary &LS) {

  /*
   * Fetch step value of induction variable
   */
  auto bbs = LS.getBasicBlocks();
  auto headerSCEV = SE.getSCEV(headerPHI);
  assert(headerSCEV->getSCEVType() == SCEVTypes::scAddRecExpr);
  auto stepSCEV = cast<SCEVAddRecExpr>(headerSCEV)->getStepRecurrence(SE);

  auto isLoopInvariant = [&bbs](Value *stepValue) -> bool {
    if (isa<ConstantInt>(stepValue)) return true;
    if (auto stepArgument = dyn_cast<Argument>(stepValue)) return true;
    if (auto stepInst = dyn_cast<Instruction>(stepValue)) {
      if (bbs.find(stepInst->getParent()) == bbs.end()) return true;
    }
    return false;
  };
  auto areLoopInvariant = [&bbs, &isLoopInvariant](std::set<Value *> &stepValues) -> bool {
    for (auto stepValue : stepValues) if (!isLoopInvariant(stepValue)) return false;
    return true;
  };

  switch (stepSCEV->getSCEVType()) {
    case SCEVTypes::scConstant:
      this->stepSize = cast<SCEVConstant>(stepSCEV)->getValue();
      break;
    case SCEVTypes::scUnknown:

      /*
       * Ensure the value is loop invariant
       */
      this->stepSize = cast<SCEVUnknown>(stepSCEV)->getValue();
      break;
    case SCEVTypes::scAddExpr:
    case SCEVTypes::scMulExpr:
    case SCEVTypes::scSMaxExpr:
    case SCEVTypes::scSMinExpr:
    case SCEVTypes::scUMaxExpr:
    case SCEVTypes::scUMinExpr:
      break;
    default:
      break;
  }

  if (this->stepSize != nullptr && !isLoopInvariant(this->stepSize)) {
    this->stepSize = nullptr;
  }
}

std::set<Value *> InductionVariable::deriveComposableStepValuesFromSCEV(const SCEV *startSCEV) {
  std::set<Value *> scevValues;
  std::queue<const SCEV *> scevs;
  scevs.push(startSCEV);

  while (!scevs.empty()) {
    auto scev = scevs.front();
    scevs.pop();

    const SCEVCommutativeExpr *commSCEV;
    switch (scev->getSCEVType()) {
      case SCEVTypes::scConstant:
        scevValues.insert(cast<SCEVConstant>(scev)->getValue());
        break;
      case SCEVTypes::scAddExpr:
      case SCEVTypes::scMulExpr:
        commSCEV = cast<SCEVCommutativeExpr>(scev);
        for (auto opI = 0; opI < commSCEV->getNumOperands(); ++opI) {
          scevs.push(commSCEV->getOperand(opI));
        }
        break;;
      case SCEVTypes::scUnknown:

        /*
        * Ensure the value is loop invariant
        */
        scevValues.insert(cast<SCEVUnknown>(scev)->getValue());
        break;
      default:

        /*
         * Some component of the SCEV isn't understand, so do not claim it can be derived
         */
        return std::set<Value *>();
    }
  }

  return scevValues;
}

/*
 * LoopGoverningIVUtility implementation
 */

LoopGoverningIVAttribution::LoopGoverningIVAttribution (InductionVariable &iv, SCC &scc, std::vector<BasicBlock *> &exitBlocks)
  : IV{iv}, scc{scc}, headerCmp{nullptr}, conditionValueDerivation{}, isWellFormed{false} {

  /*
   * To understand how to transform the loop governing condition, it is far simpler to
   * know the sign of the step size at compile time. Extra overhead is necessary if this
   * is only known at runtime, and that enhancement has yet to be made
   */
  if (iv.getSimpleValueOfStepSize() && !isa<ConstantInt>(iv.getSimpleValueOfStepSize())) return;

  auto headerPHI = iv.getHeaderPHI();
  auto &ivInstructions = iv.getAllInstructions();

  /*
   * Fetch the header terminator.
   */
  auto headerTerminator = headerPHI->getParent()->getTerminator();
  if (!isa<BranchInst>(headerTerminator)) return;
  this->headerBr = cast<BranchInst>(headerTerminator);

  /*
   * Check this is a conditional branch.
   * If it isn't, then we don't handle this type of loops (e.g., do-while).
   */
  if (!headerBr->isConditional()){
    return ;
  }

  /*
   * Fetch the condition of the conditional branch
   */
  auto headerCondition = headerBr->getCondition();
  if (!isa<CmpInst>(headerCondition)) return;

  this->headerCmp = cast<CmpInst>(headerCondition);
  auto opL = headerCmp->getOperand(0), opR = headerCmp->getOperand(1);
  if (!(opL == headerPHI ^ opR == headerPHI)) return;
  this->conditionValue = opL == headerPHI ? opR : opL;

  std::set<BasicBlock *> exitBlockSet(exitBlocks.begin(), exitBlocks.end());
  if (exitBlockSet.find(headerBr->getSuccessor(0)) != exitBlockSet.end()) {
    this->exitBlock = headerBr->getSuccessor(0);
  } else if (exitBlockSet.find(headerBr->getSuccessor(1)) != exitBlockSet.end()) {
    this->exitBlock = headerBr->getSuccessor(1);
  } else return ;

  if (scc.isInternal(conditionValue)) {
    std::queue<Instruction *> conditionDerivation;
    assert(isa<Instruction>(conditionValue)
      && "An internal value to an IV's SCC must be an instruction!");
    conditionDerivation.push(cast<Instruction>(conditionValue));

    while (!conditionDerivation.empty()) {
      auto value = conditionDerivation.front();
      conditionDerivation.pop();

      for (auto edge : scc.fetchNode(value)->getIncomingEdges()) {
        auto outgoingValue = edge->getOutgoingT();
        if (scc.isInternal(outgoingValue)) {
          assert(isa<Instruction>(outgoingValue)
            && "An internal value to an IV's SCC must be an instruction!");
          auto outgoingInst = cast<Instruction>(outgoingValue);

          /*
           * The exit condition value cannot be itself derived from the induction variable 
           */
          if (ivInstructions.find(outgoingInst) != ivInstructions.end()) return;
          conditionValueDerivation.insert(outgoingInst);
        }
      }
    }
  }

  for (auto nodePair : this->scc.internalNodePairs()) {
    auto value = nodePair.first;
    if (auto inst = dyn_cast<Instruction>(value)) {
      if (ivInstructions.find(inst) != ivInstructions.end()) continue;
      if (conditionValueDerivation.find(inst) != conditionValueDerivation.end()) continue;

      if (auto cmp = dyn_cast<CmpInst>(value)) {
        if (cmp == headerCmp) continue;
      } else if (auto br = dyn_cast<BranchInst>(value)) {
        if (br == headerBr) continue;
        if (br->isUnconditional()) continue;
      } else if (isa<GetElementPtrInst>(inst) || isa<PHINode>(inst)) {
        continue;
      }
    }

    return;
  }

  isWellFormed = true;
}

/*
 * IVUtility implementation
 */

PHINode *IVUtility::createChunkPHI (BasicBlock *preheaderB, BasicBlock *headerB, Type *chunkPHIType, Value *chunkSize) {

  // TODO: Add asserts to ensure the basic blocks/terminators are well formed

  std::vector<BasicBlock *> headerPreds(pred_begin(headerB), pred_end(headerB));
  IRBuilder<> headerBuilder(headerB->getFirstNonPHIOrDbgOrLifetime());
  auto chunkPHI = headerBuilder.CreatePHI(chunkPHIType, headerPreds.size());
  auto zeroValueForChunking = ConstantInt::get(chunkPHIType, 0);
  auto onesValueForChunking = ConstantInt::get(chunkPHIType, 1);

  for (auto B : headerPreds) {
    IRBuilder<> latchBuilder(B->getTerminator());

    if (preheaderB == B) {
      chunkPHI->addIncoming(zeroValueForChunking, B);
    } else {
      auto chunkIncrement = latchBuilder.CreateAdd(chunkPHI, onesValueForChunking);
      auto isChunkCompleted = latchBuilder.CreateICmp(CmpInst::Predicate::ICMP_EQ, chunkIncrement, chunkSize);
      auto chunkWrap = latchBuilder.CreateSelect(isChunkCompleted, zeroValueForChunking, chunkIncrement, "chunkWrap");
      chunkPHI->addIncoming(chunkWrap, B);
    }
  }

  return chunkPHI;
}

void IVUtility::chunkInductionVariablePHI(
  BasicBlock *preheaderBlock,
  PHINode *ivPHI,
  PHINode *chunkPHI,
  Value *chunkStepSize) {

  for (auto i = 0; i < ivPHI->getNumIncomingValues(); ++i) {
    auto B = ivPHI->getIncomingBlock(i);
    IRBuilder<> latchBuilder(B->getTerminator());
    if (preheaderBlock == B) continue;

    auto chunkIncomingIdx = chunkPHI->getBasicBlockIndex(B);
    Value *isChunkCompleted = cast<SelectInst>(chunkPHI->getIncomingValue(chunkIncomingIdx))->getCondition();

    /*
      * Iterate to next chunk if necessary
      */
    ivPHI->setIncomingValue(i, latchBuilder.CreateSelect(
      isChunkCompleted,
      latchBuilder.CreateAdd(ivPHI->getIncomingValue(i), chunkStepSize),
      ivPHI->getIncomingValue(i),
      "nextStepOrNextChunk"
    ));

  }
}

Value *IVUtility::composeStepSizeValue (
  InductionVariable &IV,
  IRBuilder<> builder
) {
  assert(IV.getSimpleValueOfStepSize() == nullptr &&
    "The induction variable step size is simple and does not need to be re-composed");


  auto isSimpleValue = [&](const SCEV *scev) -> bool {
    switch (scev->getSCEVType()) {
      case SCEVTypes::scConstant:
      case SCEVTypes::scUnknown:
        return true;
      default:
        return false;
    }
  };

  std::vector<const SCEVCommutativeExpr *> chainOfCompositions;
  std::queue<const SCEVCommutativeExpr *> scevs;
  // scevs.push(cast<SCEVCommutativeExpr>(compositeSCEV));
  while (!scevs.empty()) {
    auto scev = scevs.front();
    scevs.pop();

    chainOfCompositions.push_back(scev);
    switch (scev->getSCEVType()) {
      case SCEVTypes::scAddExpr:
      case SCEVTypes::scMulExpr:
        for (auto opI = 0; opI < scev->getNumOperands(); ++opI) {
          if (auto commSCEV = dyn_cast<SCEVCommutativeExpr>(scev->getOperand(opI))) {
            scevs.push(commSCEV);
          } else assert(isSimpleValue(scev->getOperand(opI)));
        }
        break;
      default:
        assert(false && "The induction variable step size isn't understood");
        return nullptr;
    }
  }

  // TODO: Compute chain
  return nullptr;
}

/*
 * LoopGoverningIVUtility implementation
 */

LoopGoverningIVUtility::LoopGoverningIVUtility (InductionVariable &IV, LoopGoverningIVAttribution &attribution)
  : attribution{attribution}, conditionValueOrderedDerivation{}, flipOperandsToUseNonStrictPredicate{false} {

  condition = attribution.getHeaderCmpInst();
  this->doesOriginalCmpInstHaveIVAsLeftOperand = condition->getOperand(0) == IV.getHeaderPHI();

  auto &conditionValueDerivationSet = attribution.getConditionValueDerivation();
  for (auto &I : *condition->getParent()) {
    if (conditionValueDerivationSet.find(&I) == conditionValueDerivationSet.end()) continue;
    conditionValueOrderedDerivation.push_back(&I);
  }

  assert(IV.getSimpleValueOfStepSize() && isa<ConstantInt>(IV.getSimpleValueOfStepSize()));
  bool isStepValuePositive = cast<ConstantInt>(IV.getSimpleValueOfStepSize())->getValue().isStrictlyPositive();
  bool conditionExitsOnTrue = attribution.getHeaderBrInst()->getSuccessor(0) == attribution.getExitBlockFromHeader();
  auto exitPredicate = conditionExitsOnTrue ? condition->getPredicate() : condition->getInversePredicate();
  // errs() << "Exit predicate before operand check: " << exitPredicate << "\n";
  exitPredicate = doesOriginalCmpInstHaveIVAsLeftOperand ? exitPredicate : CmpInst::getSwappedPredicate(exitPredicate);
  this->flipOperandsToUseNonStrictPredicate = !doesOriginalCmpInstHaveIVAsLeftOperand;
  // errs() << "Exit predicate after: " << exitPredicate << "\n";

  // condition->print(errs() << "Condition (exits on true: " << conditionExitsOnTrue << "): "); errs() << "\n";

  switch (exitPredicate) {
    case CmpInst::Predicate::ICMP_NE:
      // This predicate is non-strict and will result in either 0 or 1 iteration(s)
      this->nonStrictPredicate = exitPredicate;
      break;
    case CmpInst::Predicate::ICMP_EQ:
      // This predicate is strict and needs to be extended to LTE/GTE to catch jumping past the exiting value
      this->nonStrictPredicate = isStepValuePositive
        ? CmpInst::Predicate::ICMP_UGE : CmpInst::Predicate::ICMP_ULE;
      break;
    case CmpInst::Predicate::ICMP_SLE:
    case CmpInst::Predicate::ICMP_SLT:
    case CmpInst::Predicate::ICMP_ULT:
    case CmpInst::Predicate::ICMP_ULE:
      // This predicate is non-strict. We simply assert that the step value has the expected sign

      // HACK: while it is technically correct to increment with a less than exit condition, yielding 0 or 1 iteration,
      // it would break under assumptions that further recurrences of the IV can be checked on this condition
      // Our parallelization schemes make that assumption, hence the assert here
      assert(!isStepValuePositive && "IV step value is not compatible with exit condition!");
      this->nonStrictPredicate = exitPredicate;
      break;
    case CmpInst::Predicate::ICMP_UGT:
    case CmpInst::Predicate::ICMP_UGE:
    case CmpInst::Predicate::ICMP_SGT:
    case CmpInst::Predicate::ICMP_SGE:
      // This predicate is non-strict. We simply assert that the step value has the expected sign

      // HACK: while it is technically correct to decrement with a greater than exit condition, yielding 0 or 1 iteration,
      // it would break under assumptions that further recurrences of the IV can be checked on this condition
      // Our parallelization schemes make that assumption, hence the assert here
      assert(isStepValuePositive && "IV step value is not compatible with exit condition!");
      this->nonStrictPredicate = exitPredicate;
      break;
  }

}

void LoopGoverningIVUtility::updateConditionAndBranchToCatchIteratingPastExitValue(
  CmpInst *cmpToUpdate,
  BranchInst *branchInst,
  BasicBlock *exitBlock) {

  if (flipOperandsToUseNonStrictPredicate) {
    auto opL = cmpToUpdate->getOperand(0);
    auto opR = cmpToUpdate->getOperand(1);
    cmpToUpdate->setOperand(0, opR);
    cmpToUpdate->setOperand(1, opL);
  }
  cmpToUpdate->setPredicate(nonStrictPredicate);

  if (branchInst->getSuccessor(0) != exitBlock) {
    branchInst->setSuccessor(1, branchInst->getSuccessor(0));
    branchInst->setSuccessor(0, exitBlock);
  }
}

void LoopGoverningIVUtility::cloneConditionalCheckFor(
  Value *recurrenceOfIV,
  Value *clonedCompareValue,
  BasicBlock *continueBlock,
  BasicBlock *exitBlock,
  IRBuilder<> &cloneBuilder) {

  Value *cmpInst;
  cmpInst = cloneBuilder.CreateICmp(nonStrictPredicate, recurrenceOfIV, clonedCompareValue);
  cloneBuilder.CreateCondBr(cmpInst, exitBlock, continueBlock);
}
