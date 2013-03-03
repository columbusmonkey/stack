// This pass kills unreachable (dead) code by exploiting undefined behavior.
// The basic idea is that given a reachable statement s, if it always blows up
// another reachable statement t (i.e., triggering t's undefined behavior),
// then s is actually "dead" in terms of undefined behavior.

#define DEBUG_TYPE "anti-dce"
#include "AntiFunctionPass.h"
#include <llvm/Analysis/Dominators.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CFG.h>
#include <llvm/Support/Debug.h>

using namespace llvm;

namespace {

struct AntiDCE: AntiFunctionPass {
	static char ID;
	AntiDCE() : AntiFunctionPass(ID) {}

	virtual void getAnalysisUsage(AnalysisUsage &AU) const {
		AntiFunctionPass::getAnalysisUsage(AU);
		AU.addPreserved<DominatorTree>();
		AU.addPreserved<PostDominatorTree>();
	}

	virtual bool runOnAntiFunction(Function &);

private:
	bool shouldCheck(BasicBlock *BB);
	int shouldKeepCode(BasicBlock *BB);
	void report(BasicBlock *BB);
	void markAsDead(BasicBlock *BB);
};

} // anonymous namespace

bool AntiDCE::shouldCheck(BasicBlock *BB) {
	// Skip exception handlers.
	if (BB->isLandingPad())
		return false;
	// Ignore unreachable blocks, often from BUG_ON() or assert().
	if (isa<UnreachableInst>(BB->getTerminator()))
		return false;
	// Ignore empty default.
	if (BasicBlock *Pred = BB->getSinglePredecessor()) {
		if (SwitchInst *SI = dyn_cast<SwitchInst>(Pred->getTerminator()))
			if (SI->getDefaultDest() == BB)
				return false;
	}
	if (isa<TerminatorInst>(BB->getFirstInsertionPt()))
		return false;
	// BB may become unreachable after marking some block as unreachable.
	if (!DT->isReachableFromEntry(BB))
		return false;
	for (BasicBlock::iterator i = BB->begin(), e = BB->end(); i != e; ++i) {
		if (Diagnostic::hasSingleDebugLocation(i))
			return true;
	}
	return false;
}

bool AntiDCE::runOnAntiFunction(Function &F) {
	bool Changed = false;
	for (Function::iterator i = F.begin(), e = F.end(); i != e; ++i) {
		BasicBlock *BB = i;
		if (!shouldCheck(BB))
			continue;
		int Keep;
		if (SMTFork() == 0)
			Keep = shouldKeepCode(BB);
		SMTJoin(&Keep);
		if (Keep)
			continue;
		report(BB);
		Changed = true;
		markAsDead(BB);
		// Update if any optimization performed.
		recalculate(F);
	}
	return Changed;
}

int AntiDCE::shouldKeepCode(BasicBlock *BB) {
	SMTSolver SMT(false);
	ValueGen VG(*DL, SMT);
	// Compute path condition.
	PathGen PG(VG, Backedges, *DT);
	SMTExpr R = PG.get(BB);
	// Ignore dead path.
	if (SMT.query(R) == SMT_UNSAT)
		return 1;
	// Collect bug assertions.
	SMTExpr Delta = getDeltaForBlock(BB, VG);
	if (!Delta)
		return 1;
	SMTStatus Status = queryWithDelta(R, Delta, VG);
	SMT.decref(Delta);
	if (Status == SMT_UNSAT)
		return 0;
	return 1;
}

void AntiDCE::report(BasicBlock *BB) {
	// Prove BB is dead; output warning message.
	Diag.bug(DEBUG_TYPE);
	Diag << "model: |\n";
	if (auto Pred = BB->getUniquePredecessor()) {
		if (auto BI = dyn_cast<BranchInst>(Pred->getTerminator()))
			if (auto Cond = dyn_cast<Instruction>(BI->getCondition()))
				Diag << *Cond << "\n  -->  "
				     << ((BI->getSuccessor(0) == BB) ? "false" : "true")
				     << "\n  ************************************************************\n";
	}
	Diag << "  " << BB->getName() << ":\n";
	for (Instruction &I: *BB)
		Diag << I << '\n';
	for (Instruction &I: *BB) {
		if (!I.getDebugLoc().isUnknown()) {
			Diag.backtrace(&I);
			break;
		}
	}
	printMinimalAssertions();
}

void AntiDCE::markAsDead(BasicBlock *BB) {
	// Remove BB from successors.
	std::vector<BasicBlock *> Succs(succ_begin(BB), succ_end(BB));
	for (unsigned i = 0, e = Succs.size(); i != e; ++i)
		Succs[i]->removePredecessor(BB);
	// Empty BB.
	for (BasicBlock::iterator i = BB->begin(), e = BB->end(); i != e; ) {
		Instruction *I = i++;
		Type *T = I->getType();
		if (!I->use_empty())
			I->replaceAllUsesWith(UndefValue::get(T));
		I->eraseFromParent();
	}
	// Mark it as unreachable.
	new UnreachableInst(BB->getContext(), BB);
}

char AntiDCE::ID;

static RegisterPass<AntiDCE>
X("anti-dce", "Anti Dead Code Elimination");
