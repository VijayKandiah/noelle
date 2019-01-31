/*
 * Copyright 2016 - 2019  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "SystemHeaders.hpp"
#include "Hot.hpp"

using namespace llvm ;

Hot::Hot ()
  : moduleNumberOfInstructionsExecuted{0}
  {
  return ;
}

void Hot::setBasicBlockInvocations (BasicBlock *bb, uint64_t invocations){

  /*
   * Check if bb is the entry point of a function.
   */
  auto f = bb->getParent();
  auto& entryBB = f->getEntryBlock();
  if (&entryBB == bb){

    /*
     * Insert the number of invocations of a function.
     */
    this->functionInvocations[f] = invocations;
  }

  /*
   * Insert the number of invocations of the basic block bb.
   */
  this->bbInvocations[bb] = invocations;

  return ;
}

uint64_t Hot::getBasicBlockInvocations (BasicBlock *bb) {
  auto inv = this->bbInvocations[bb];

  return inv;
}
      
uint64_t Hot::getBasicBlockDynamicInstructions (BasicBlock *bb) {
  auto inv = this->getBasicBlockInvocations(bb);
  
  auto bbLength = std::distance(bb->begin(), bb->end());

  return inv * bbLength;
}

double Hot::getBranchFrequency (BasicBlock *sourceBB, BasicBlock *targetBB) {
  auto v1 = (double )this->bbInvocations[sourceBB];
  auto v2 = (double )this->bbInvocations[targetBB];

  return v2 / v1;
}
 
uint64_t Hot::getFunctionDynamicInstructions (Function *f){
  auto insts = this->functionInstructions[f];

  return insts;
}

uint64_t Hot::getModuleInstructionExecuted (void) const {
  return this->moduleNumberOfInstructionsExecuted;
}
      
void Hot::computeProgramInvocations (void){

  /*
   * Compute the total number of instructions executed.
   */
  for (auto pairs : this->bbInvocations){

    /*
     * Fetch the current basic block
     */
    auto bb = pairs.first;
    auto bbInv = pairs.second;

    /*
     * Fetch the number of invocations of the basic block and its length.
     */
    auto totalBBInsts = this->getBasicBlockInvocations(bb);
    auto bbLength = std::distance(bb->begin(), bb->end());

    /*
     * Update the module counter
     */
    this->moduleNumberOfInstructionsExecuted += (totalBBInsts * bbLength);
  }

  /*
   * Compute the total number of instructions executed by each function.
   * Each call instructions is considered one; so callee instructions are not considered.
   */
  for (auto pairs : this->functionInvocations){

    /*
     * Fetch the function.
     */
    auto f = pairs.first;
    
    /*
     * Consider all basic blocks.
     */
    uint64_t c = 0;
    for (auto& bb : *f){
      c += this->getBasicBlockDynamicInstructions(&bb);
    }
    this->functionInstructions[f] = c;
  }
  

  return ;
}