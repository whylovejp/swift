//===--- AccessSummaryAnalysis.h - SIL Access Summary Analysis --*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements an interprocedural analysis pass that summarizes
// the formal accesses that a function makes to its address-type arguments.
// These summaries are used to statically diagnose violations of exclusive
// accesses for noescape closures.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_ACCESS_SUMMARY_ANALYSIS_H_
#define SWIFT_SILOPTIMIZER_ANALYSIS_ACCESS_SUMMARY_ANALYSIS_H_

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/BottomUpIPAnalysis.h"
#include "swift/SILOptimizer/Utils/IndexTrie.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class AccessSummaryAnalysis : public BottomUpIPAnalysis {
public:
  /// Summarizes the accesses that a function begins on an argument.
  class ArgumentSummary {
  private:
    /// The kind of access begun on the argument.
    /// 'None' means no access performed.
    Optional<SILAccessKind> Kind = None;

    /// The location of the access. Used for diagnostics.
    SILLocation AccessLoc = SILLocation((Expr *)nullptr);

  public:
    Optional<SILAccessKind> getAccessKind() const { return Kind; }

    SILLocation getAccessLoc() const { return AccessLoc; }

    /// The lattice operation on argument summaries.
    bool mergeWith(const ArgumentSummary &other);

    /// Merge in an access to the argument of the given kind at the given
    /// location. Returns true if the merge caused the summary to change.
    bool mergeWith(SILAccessKind otherKind, SILLocation otherLoc);

    /// Returns a description of the summary. For debugging and testing
    /// purposes.
    StringRef getDescription() const;
  };

  /// Summarizes the accesses that a function begins on its arguments.
  class FunctionSummary {
  private:
    llvm::SmallVector<ArgumentSummary, 6> ArgAccesses;

  public:
    FunctionSummary(unsigned argCount) : ArgAccesses(argCount) {}

    /// Returns of summary of the the function accesses that argument at the
    /// given index.
    ArgumentSummary &getAccessForArgument(unsigned argument) {
      return ArgAccesses[argument];
    }

    const ArgumentSummary &getAccessForArgument(unsigned argument) const {
      return ArgAccesses[argument];
    }

    /// Returns the number of argument in the summary.
    unsigned getArgumentCount() const { return ArgAccesses.size(); }
  };

  friend raw_ostream &operator<<(raw_ostream &os,
                                 const FunctionSummary &summary);

  class FunctionInfo;
  /// Records a flow of a caller's argument to a called function.
  /// This flow is used to iterate the interprocedural analysis to a fixpoint.
  struct ArgumentFlow {
    /// The index of the argument in the caller.
    const unsigned CallerArgumentIndex;

    /// The index of the argument in the callee.
    const unsigned CalleeArgumentIndex;

    FunctionInfo *const CalleeFunctionInfo;
  };

  /// Records the summary and argument flows for a given function.
  /// Used by the BottomUpIPAnalysis to propagate information
  /// from callees to callers.
  class FunctionInfo : public FunctionInfoBase<FunctionInfo> {
  private:
    FunctionSummary FS;

    SILFunction *F;

    llvm::SmallVector<ArgumentFlow, 8> RecordedArgumentFlows;

  public:
    FunctionInfo(SILFunction *F) : FS(F->getArguments().size()), F(F) {}

    SILFunction *getFunction() const { return F; }

    ArrayRef<ArgumentFlow> getArgumentFlows() const {
      return RecordedArgumentFlows;
    }

    const FunctionSummary &getSummary() const { return FS; }
    FunctionSummary &getSummary() { return FS; }

    /// Record a flow of an argument in this function to a callee.
    void recordFlow(const ArgumentFlow &flow) {
      flow.CalleeFunctionInfo->addCaller(this, nullptr);
      RecordedArgumentFlows.push_back(flow);
    }
  };

private:
  /// Maps functions to the information the analysis keeps for each function.
  llvm::DenseMap<SILFunction *, FunctionInfo *> FunctionInfos;

  llvm::SpecificBumpPtrAllocator<FunctionInfo> Allocator;

  /// A trie of integer indices that gives pointer identity to a path of
  /// projections. This is shared between all functions in the module.
  IndexTrieNode *SubPathTrie;

public:
  AccessSummaryAnalysis() : BottomUpIPAnalysis(AnalysisKind::AccessSummary) {
    SubPathTrie = new IndexTrieNode();
  }

  ~AccessSummaryAnalysis() {
    delete SubPathTrie;
  }

  /// Returns a summary of the accesses performed by the given function.
  const FunctionSummary &getOrCreateSummary(SILFunction *Fn);

  IndexTrieNode *getSubPathTrieRoot() {
    return SubPathTrie;
  }

  virtual void initialize(SILPassManager *PM) override {}
  virtual void invalidate() override;
  virtual void invalidate(SILFunction *F, InvalidationKind K) override;
  virtual void notifyAddFunction(SILFunction *F) override {}
  virtual void notifyDeleteFunction(SILFunction *F) override {
    invalidate(F, InvalidationKind::Nothing);
  }
  virtual void invalidateFunctionTables() override {}

  static bool classof(const SILAnalysis *S) {
    return S->getKind() == AnalysisKind::AccessSummary;
  }

private:
  typedef BottomUpFunctionOrder<FunctionInfo> FunctionOrder;

  /// Returns the BottomUpIPAnalysis information for the given function.
  FunctionInfo *getFunctionInfo(SILFunction *F);

  /// Summarizes the given function and iterates the interprocedural analysis
  /// to a fixpoint.
  void recompute(FunctionInfo *initial);

  /// Propagate the access summary from the argument of a called function
  /// to the caller.
  bool propagateFromCalleeToCaller(FunctionInfo *callerInfo,
                                   ArgumentFlow site);

  /// Summarize the given function and schedule it for interprocedural
  /// analysis.
  void processFunction(FunctionInfo *info, FunctionOrder &order);

  /// Summarize how the function uses the given argument.
  void processArgument(FunctionInfo *info, SILFunctionArgument *argment,
                        ArgumentSummary &summary, FunctionOrder &order);

  /// Summarize a partial_apply instruction.
  void processPartialApply(FunctionInfo *callerInfo,
                           unsigned callerArgumentIndex,
                           PartialApplyInst *apply,
                           Operand *applyArgumentOperand, FunctionOrder &order);

  /// Summarize apply or try_apply
  void processFullApply(FunctionInfo *callerInfo, unsigned callerArgumentIndex,
                        FullApplySite apply, Operand *argumentOperand,
                        FunctionOrder &order);

  /// Summarize a call site and schedule it for interprocedural analysis.
  void processCall(FunctionInfo *callerInfo, unsigned callerArgumentIndex,
                   SILFunction *calledFunction, unsigned argumentIndex,
                   FunctionOrder &order);
};

} // end namespace swift

#endif

