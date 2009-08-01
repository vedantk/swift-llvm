//===-- ARMConstantIslandPass.cpp - ARM constant islands --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that splits the constant pool up into 'islands'
// which are scattered through-out the function.  This is required due to the
// limited pc-relative displacements that ARM has.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "arm-cp-islands"
#include "ARM.h"
#include "ARMAddressingModes.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMInstrInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
using namespace llvm;

STATISTIC(NumCPEs,     "Number of constpool entries");
STATISTIC(NumSplit,    "Number of uncond branches inserted");
STATISTIC(NumCBrFixed, "Number of cond branches fixed");
STATISTIC(NumUBrFixed, "Number of uncond branches fixed");
STATISTIC(NumTBs,      "Number of table branches generated");

namespace {
  /// ARMConstantIslands - Due to limited PC-relative displacements, ARM
  /// requires constant pool entries to be scattered among the instructions
  /// inside a function.  To do this, it completely ignores the normal LLVM
  /// constant pool; instead, it places constants wherever it feels like with
  /// special instructions.
  ///
  /// The terminology used in this pass includes:
  ///   Islands - Clumps of constants placed in the function.
  ///   Water   - Potential places where an island could be formed.
  ///   CPE     - A constant pool entry that has been placed somewhere, which
  ///             tracks a list of users.
  class VISIBILITY_HIDDEN ARMConstantIslands : public MachineFunctionPass {
    /// BBSizes - The size of each MachineBasicBlock in bytes of code, indexed
    /// by MBB Number.  The two-byte pads required for Thumb alignment are
    /// counted as part of the following block (i.e., the offset and size for
    /// a padded block will both be ==2 mod 4).
    std::vector<unsigned> BBSizes;

    /// BBOffsets - the offset of each MBB in bytes, starting from 0.
    /// The two-byte pads required for Thumb alignment are counted as part of
    /// the following block.
    std::vector<unsigned> BBOffsets;

    /// WaterList - A sorted list of basic blocks where islands could be placed
    /// (i.e. blocks that don't fall through to the following block, due
    /// to a return, unreachable, or unconditional branch).
    std::vector<MachineBasicBlock*> WaterList;

    /// CPUser - One user of a constant pool, keeping the machine instruction
    /// pointer, the constant pool being referenced, and the max displacement
    /// allowed from the instruction to the CP.
    struct CPUser {
      MachineInstr *MI;
      MachineInstr *CPEMI;
      unsigned MaxDisp;
      bool NegOk;
      bool IsSoImm;
      CPUser(MachineInstr *mi, MachineInstr *cpemi, unsigned maxdisp,
             bool neg, bool soimm)
        : MI(mi), CPEMI(cpemi), MaxDisp(maxdisp), NegOk(neg), IsSoImm(soimm) {}
    };

    /// CPUsers - Keep track of all of the machine instructions that use various
    /// constant pools and their max displacement.
    std::vector<CPUser> CPUsers;

    /// CPEntry - One per constant pool entry, keeping the machine instruction
    /// pointer, the constpool index, and the number of CPUser's which
    /// reference this entry.
    struct CPEntry {
      MachineInstr *CPEMI;
      unsigned CPI;
      unsigned RefCount;
      CPEntry(MachineInstr *cpemi, unsigned cpi, unsigned rc = 0)
        : CPEMI(cpemi), CPI(cpi), RefCount(rc) {}
    };

    /// CPEntries - Keep track of all of the constant pool entry machine
    /// instructions. For each original constpool index (i.e. those that
    /// existed upon entry to this pass), it keeps a vector of entries.
    /// Original elements are cloned as we go along; the clones are
    /// put in the vector of the original element, but have distinct CPIs.
    std::vector<std::vector<CPEntry> > CPEntries;

    /// ImmBranch - One per immediate branch, keeping the machine instruction
    /// pointer, conditional or unconditional, the max displacement,
    /// and (if isCond is true) the corresponding unconditional branch
    /// opcode.
    struct ImmBranch {
      MachineInstr *MI;
      unsigned MaxDisp : 31;
      bool isCond : 1;
      int UncondBr;
      ImmBranch(MachineInstr *mi, unsigned maxdisp, bool cond, int ubr)
        : MI(mi), MaxDisp(maxdisp), isCond(cond), UncondBr(ubr) {}
    };

    /// ImmBranches - Keep track of all the immediate branch instructions.
    ///
    std::vector<ImmBranch> ImmBranches;

    /// PushPopMIs - Keep track of all the Thumb push / pop instructions.
    ///
    SmallVector<MachineInstr*, 4> PushPopMIs;

    /// T2JumpTables - Keep track of all the Thumb2 jumptable instructions.
    SmallVector<MachineInstr*, 4> T2JumpTables;

    /// HasFarJump - True if any far jump instruction has been emitted during
    /// the branch fix up pass.
    bool HasFarJump;

    const TargetInstrInfo *TII;
    const ARMSubtarget *STI;
    ARMFunctionInfo *AFI;
    bool isThumb;
    bool isThumb1;
    bool isThumb2;
  public:
    static char ID;
    ARMConstantIslands() : MachineFunctionPass(&ID) {}

    virtual bool runOnMachineFunction(MachineFunction &MF);

    virtual const char *getPassName() const {
      return "ARM constant island placement and branch shortening pass";
    }

  private:
    void DoInitialPlacement(MachineFunction &MF,
                            std::vector<MachineInstr*> &CPEMIs);
    CPEntry *findConstPoolEntry(unsigned CPI, const MachineInstr *CPEMI);
    void InitialFunctionScan(MachineFunction &MF,
                             const std::vector<MachineInstr*> &CPEMIs);
    MachineBasicBlock *SplitBlockBeforeInstr(MachineInstr *MI);
    void UpdateForInsertedWaterBlock(MachineBasicBlock *NewBB);
    void AdjustBBOffsetsAfter(MachineBasicBlock *BB, int delta);
    bool DecrementOldEntry(unsigned CPI, MachineInstr* CPEMI);
    int LookForExistingCPEntry(CPUser& U, unsigned UserOffset);
    bool LookForWater(CPUser&U, unsigned UserOffset,
                      MachineBasicBlock** NewMBB);
    MachineBasicBlock* AcceptWater(MachineBasicBlock *WaterBB,
                        std::vector<MachineBasicBlock*>::iterator IP);
    void CreateNewWater(unsigned CPUserIndex, unsigned UserOffset,
                      MachineBasicBlock** NewMBB);
    bool HandleConstantPoolUser(MachineFunction &MF, unsigned CPUserIndex);
    void RemoveDeadCPEMI(MachineInstr *CPEMI);
    bool RemoveUnusedCPEntries();
    bool CPEIsInRange(MachineInstr *MI, unsigned UserOffset,
                      MachineInstr *CPEMI, unsigned Disp, bool NegOk,
                      bool DoDump = false);
    bool WaterIsInRange(unsigned UserOffset, MachineBasicBlock *Water,
                        CPUser &U);
    bool OffsetIsInRange(unsigned UserOffset, unsigned TrialOffset,
                         unsigned Disp, bool NegativeOK, bool IsSoImm = false);
    bool BBIsInRange(MachineInstr *MI, MachineBasicBlock *BB, unsigned Disp);
    bool FixUpImmediateBr(MachineFunction &MF, ImmBranch &Br);
    bool FixUpConditionalBr(MachineFunction &MF, ImmBranch &Br);
    bool FixUpUnconditionalBr(MachineFunction &MF, ImmBranch &Br);
    bool UndoLRSpillRestore();
    bool OptimizeThumb2JumpTables(MachineFunction &MF);

    unsigned GetOffsetOf(MachineInstr *MI) const;
    void dumpBBs();
    void verify(MachineFunction &MF);
  };
  char ARMConstantIslands::ID = 0;
}

/// verify - check BBOffsets, BBSizes, alignment of islands
void ARMConstantIslands::verify(MachineFunction &MF) {
  assert(BBOffsets.size() == BBSizes.size());
  for (unsigned i = 1, e = BBOffsets.size(); i != e; ++i)
    assert(BBOffsets[i-1]+BBSizes[i-1] == BBOffsets[i]);
  if (!isThumb)
    return;
#ifndef NDEBUG
  for (MachineFunction::iterator MBBI = MF.begin(), E = MF.end();
       MBBI != E; ++MBBI) {
    MachineBasicBlock *MBB = MBBI;
    if (!MBB->empty() &&
        MBB->begin()->getOpcode() == ARM::CONSTPOOL_ENTRY) {
      unsigned MBBId = MBB->getNumber();
      assert((BBOffsets[MBBId]%4 == 0 && BBSizes[MBBId]%4 == 0) ||
             (BBOffsets[MBBId]%4 != 0 && BBSizes[MBBId]%4 != 0));
    }
  }
#endif
}

/// print block size and offset information - debugging
void ARMConstantIslands::dumpBBs() {
  for (unsigned J = 0, E = BBOffsets.size(); J !=E; ++J) {
    DOUT << "block " << J << " offset " << BBOffsets[J] <<
                            " size " << BBSizes[J] << "\n";
  }
}

/// createARMConstantIslandPass - returns an instance of the constpool
/// island pass.
FunctionPass *llvm::createARMConstantIslandPass() {
  return new ARMConstantIslands();
}

bool ARMConstantIslands::runOnMachineFunction(MachineFunction &MF) {
  MachineConstantPool &MCP = *MF.getConstantPool();

  TII = MF.getTarget().getInstrInfo();
  AFI = MF.getInfo<ARMFunctionInfo>();
  STI = &MF.getTarget().getSubtarget<ARMSubtarget>();

  isThumb = AFI->isThumbFunction();
  isThumb1 = AFI->isThumb1OnlyFunction();
  isThumb2 = AFI->isThumb2Function();

  HasFarJump = false;

  // Renumber all of the machine basic blocks in the function, guaranteeing that
  // the numbers agree with the position of the block in the function.
  MF.RenumberBlocks();

  // Thumb1 functions containing constant pools get 4-byte alignment.
  // This is so we can keep exact track of where the alignment padding goes.

  // Set default. Thumb1 function is 2-byte aligned, ARM and Thumb2 are 4-byte
  // aligned.
  AFI->setAlign(isThumb1 ? 1U : 2U);

  // Perform the initial placement of the constant pool entries.  To start with,
  // we put them all at the end of the function.
  std::vector<MachineInstr*> CPEMIs;
  if (!MCP.isEmpty()) {
    DoInitialPlacement(MF, CPEMIs);
    if (isThumb1)
      AFI->setAlign(2U);
  }

  /// The next UID to take is the first unused one.
  AFI->initConstPoolEntryUId(CPEMIs.size());

  // Do the initial scan of the function, building up information about the
  // sizes of each block, the location of all the water, and finding all of the
  // constant pool users.
  InitialFunctionScan(MF, CPEMIs);
  CPEMIs.clear();

  /// Remove dead constant pool entries.
  RemoveUnusedCPEntries();

  // Iteratively place constant pool entries and fix up branches until there
  // is no change.
  bool MadeChange = false;
  while (true) {
    bool Change = false;
    for (unsigned i = 0, e = CPUsers.size(); i != e; ++i)
      Change |= HandleConstantPoolUser(MF, i);
    DEBUG(dumpBBs());
    for (unsigned i = 0, e = ImmBranches.size(); i != e; ++i)
      Change |= FixUpImmediateBr(MF, ImmBranches[i]);
    DEBUG(dumpBBs());
    if (!Change)
      break;
    MadeChange = true;
  }

  // Let's see if we can use tbb / tbh to do jump tables.
  MadeChange |= OptimizeThumb2JumpTables(MF);

  // After a while, this might be made debug-only, but it is not expensive.
  verify(MF);

  // If LR has been forced spilled and no far jumps (i.e. BL) has been issued.
  // Undo the spill / restore of LR if possible.
  if (isThumb && !HasFarJump && AFI->isLRSpilledForFarJump())
    MadeChange |= UndoLRSpillRestore();

  BBSizes.clear();
  BBOffsets.clear();
  WaterList.clear();
  CPUsers.clear();
  CPEntries.clear();
  ImmBranches.clear();
  PushPopMIs.clear();
  T2JumpTables.clear();

  return MadeChange;
}

/// DoInitialPlacement - Perform the initial placement of the constant pool
/// entries.  To start with, we put them all at the end of the function.
void ARMConstantIslands::DoInitialPlacement(MachineFunction &MF,
                                        std::vector<MachineInstr*> &CPEMIs) {
  // Create the basic block to hold the CPE's.
  MachineBasicBlock *BB = MF.CreateMachineBasicBlock();
  MF.push_back(BB);

  // Add all of the constants from the constant pool to the end block, use an
  // identity mapping of CPI's to CPE's.
  const std::vector<MachineConstantPoolEntry> &CPs =
    MF.getConstantPool()->getConstants();

  const TargetData &TD = *MF.getTarget().getTargetData();
  for (unsigned i = 0, e = CPs.size(); i != e; ++i) {
    unsigned Size = TD.getTypeAllocSize(CPs[i].getType());
    // Verify that all constant pool entries are a multiple of 4 bytes.  If not,
    // we would have to pad them out or something so that instructions stay
    // aligned.
    assert((Size & 3) == 0 && "CP Entry not multiple of 4 bytes!");
    MachineInstr *CPEMI =
      BuildMI(BB, DebugLoc::getUnknownLoc(), TII->get(ARM::CONSTPOOL_ENTRY))
                           .addImm(i).addConstantPoolIndex(i).addImm(Size);
    CPEMIs.push_back(CPEMI);

    // Add a new CPEntry, but no corresponding CPUser yet.
    std::vector<CPEntry> CPEs;
    CPEs.push_back(CPEntry(CPEMI, i));
    CPEntries.push_back(CPEs);
    NumCPEs++;
    DOUT << "Moved CPI#" << i << " to end of function as #" << i << "\n";
  }
}

/// BBHasFallthrough - Return true if the specified basic block can fallthrough
/// into the block immediately after it.
static bool BBHasFallthrough(MachineBasicBlock *MBB) {
  // Get the next machine basic block in the function.
  MachineFunction::iterator MBBI = MBB;
  if (next(MBBI) == MBB->getParent()->end())  // Can't fall off end of function.
    return false;

  MachineBasicBlock *NextBB = next(MBBI);
  for (MachineBasicBlock::succ_iterator I = MBB->succ_begin(),
       E = MBB->succ_end(); I != E; ++I)
    if (*I == NextBB)
      return true;

  return false;
}

/// findConstPoolEntry - Given the constpool index and CONSTPOOL_ENTRY MI,
/// look up the corresponding CPEntry.
ARMConstantIslands::CPEntry
*ARMConstantIslands::findConstPoolEntry(unsigned CPI,
                                        const MachineInstr *CPEMI) {
  std::vector<CPEntry> &CPEs = CPEntries[CPI];
  // Number of entries per constpool index should be small, just do a
  // linear search.
  for (unsigned i = 0, e = CPEs.size(); i != e; ++i) {
    if (CPEs[i].CPEMI == CPEMI)
      return &CPEs[i];
  }
  return NULL;
}

/// InitialFunctionScan - Do the initial scan of the function, building up
/// information about the sizes of each block, the location of all the water,
/// and finding all of the constant pool users.
void ARMConstantIslands::InitialFunctionScan(MachineFunction &MF,
                                 const std::vector<MachineInstr*> &CPEMIs) {
  unsigned Offset = 0;
  for (MachineFunction::iterator MBBI = MF.begin(), E = MF.end();
       MBBI != E; ++MBBI) {
    MachineBasicBlock &MBB = *MBBI;

    // If this block doesn't fall through into the next MBB, then this is
    // 'water' that a constant pool island could be placed.
    if (!BBHasFallthrough(&MBB))
      WaterList.push_back(&MBB);

    unsigned MBBSize = 0;
    for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end();
         I != E; ++I) {
      // Add instruction size to MBBSize.
      MBBSize += TII->GetInstSizeInBytes(I);

      int Opc = I->getOpcode();
      if (I->getDesc().isBranch()) {
        bool isCond = false;
        unsigned Bits = 0;
        unsigned Scale = 1;
        int UOpc = Opc;
        switch (Opc) {
        default:
          continue;  // Ignore other JT branches
        case ARM::tBR_JTr:
          // A Thumb1 table jump may involve padding; for the offsets to
          // be right, functions containing these must be 4-byte aligned.
          AFI->setAlign(2U);
          if ((Offset+MBBSize)%4 != 0)
            // FIXME: Add a pseudo ALIGN instruction instead.
            MBBSize += 2;           // padding
          continue;   // Does not get an entry in ImmBranches
        case ARM::t2BR_JT:
          T2JumpTables.push_back(I);
          continue;   // Does not get an entry in ImmBranches
        case ARM::Bcc:
          isCond = true;
          UOpc = ARM::B;
          // Fallthrough
        case ARM::B:
          Bits = 24;
          Scale = 4;
          break;
        case ARM::tBcc:
          isCond = true;
          UOpc = ARM::tB;
          Bits = 8;
          Scale = 2;
          break;
        case ARM::tB:
          Bits = 11;
          Scale = 2;
          break;
        case ARM::t2Bcc:
          isCond = true;
          UOpc = ARM::t2B;
          Bits = 20;
          Scale = 2;
          break;
        case ARM::t2B:
          Bits = 24;
          Scale = 2;
          break;
        }

        // Record this immediate branch.
        unsigned MaxOffs = ((1 << (Bits-1))-1) * Scale;
        ImmBranches.push_back(ImmBranch(I, MaxOffs, isCond, UOpc));
      }

      if (Opc == ARM::tPUSH || Opc == ARM::tPOP_RET)
        PushPopMIs.push_back(I);

      if (Opc == ARM::CONSTPOOL_ENTRY)
        continue;

      // Scan the instructions for constant pool operands.
      for (unsigned op = 0, e = I->getNumOperands(); op != e; ++op)
        if (I->getOperand(op).isCPI()) {
          // We found one.  The addressing mode tells us the max displacement
          // from the PC that this instruction permits.

          // Basic size info comes from the TSFlags field.
          unsigned Bits = 0;
          unsigned Scale = 1;
          bool NegOk = false;
          bool IsSoImm = false;

          // FIXME: Temporary workaround until I can figure out what's going on.
          unsigned Slack = T2JumpTables.empty() ? 0 : 4;
          switch (Opc) {
          default:
            llvm_unreachable("Unknown addressing mode for CP reference!");
            break;

          // Taking the address of a CP entry.
          case ARM::LEApcrel:
            // This takes a SoImm, which is 8 bit immediate rotated. We'll
            // pretend the maximum offset is 255 * 4. Since each instruction
            // 4 byte wide, this is always correct. We'llc heck for other
            // displacements that fits in a SoImm as well.
            Bits = 8;
            Scale = 4;
            NegOk = true;
            IsSoImm = true;
            break;
          case ARM::t2LEApcrel:
            Bits = 12;
            NegOk = true;
            break;
          case ARM::tLEApcrel:
            Bits = 8;
            Scale = 4;
            break;

          case ARM::LDR:
          case ARM::LDRcp:
          case ARM::t2LDRpci:
            Bits = 12;  // +-offset_12
            NegOk = true;
            break;

          case ARM::tLDRpci:
          case ARM::tLDRcp:
            Bits = 8;
            Scale = 4;  // +(offset_8*4)
            break;

          case ARM::FLDD:
          case ARM::FLDS:
            Bits = 8;
            Scale = 4;  // +-(offset_8*4)
            NegOk = true;
            break;
          }

          // Remember that this is a user of a CP entry.
          unsigned CPI = I->getOperand(op).getIndex();
          MachineInstr *CPEMI = CPEMIs[CPI];
          unsigned MaxOffs = ((1 << Bits)-1) * Scale - Slack;
          CPUsers.push_back(CPUser(I, CPEMI, MaxOffs, NegOk, IsSoImm));

          // Increment corresponding CPEntry reference count.
          CPEntry *CPE = findConstPoolEntry(CPI, CPEMI);
          assert(CPE && "Cannot find a corresponding CPEntry!");
          CPE->RefCount++;

          // Instructions can only use one CP entry, don't bother scanning the
          // rest of the operands.
          break;
        }
    }

    // In thumb mode, if this block is a constpool island, we may need padding
    // so it's aligned on 4 byte boundary.
    if (isThumb &&
        !MBB.empty() &&
        MBB.begin()->getOpcode() == ARM::CONSTPOOL_ENTRY &&
        (Offset%4) != 0)
      MBBSize += 2;

    BBSizes.push_back(MBBSize);
    BBOffsets.push_back(Offset);
    Offset += MBBSize;
  }
}

/// GetOffsetOf - Return the current offset of the specified machine instruction
/// from the start of the function.  This offset changes as stuff is moved
/// around inside the function.
unsigned ARMConstantIslands::GetOffsetOf(MachineInstr *MI) const {
  MachineBasicBlock *MBB = MI->getParent();

  // The offset is composed of two things: the sum of the sizes of all MBB's
  // before this instruction's block, and the offset from the start of the block
  // it is in.
  unsigned Offset = BBOffsets[MBB->getNumber()];

  // If we're looking for a CONSTPOOL_ENTRY in Thumb, see if this block has
  // alignment padding, and compensate if so.
  if (isThumb &&
      MI->getOpcode() == ARM::CONSTPOOL_ENTRY &&
      Offset%4 != 0)
    Offset += 2;

  // Sum instructions before MI in MBB.
  for (MachineBasicBlock::iterator I = MBB->begin(); ; ++I) {
    assert(I != MBB->end() && "Didn't find MI in its own basic block?");
    if (&*I == MI) return Offset;
    Offset += TII->GetInstSizeInBytes(I);
  }
}

/// CompareMBBNumbers - Little predicate function to sort the WaterList by MBB
/// ID.
static bool CompareMBBNumbers(const MachineBasicBlock *LHS,
                              const MachineBasicBlock *RHS) {
  return LHS->getNumber() < RHS->getNumber();
}

/// UpdateForInsertedWaterBlock - When a block is newly inserted into the
/// machine function, it upsets all of the block numbers.  Renumber the blocks
/// and update the arrays that parallel this numbering.
void ARMConstantIslands::UpdateForInsertedWaterBlock(MachineBasicBlock *NewBB) {
  // Renumber the MBB's to keep them consequtive.
  NewBB->getParent()->RenumberBlocks(NewBB);

  // Insert a size into BBSizes to align it properly with the (newly
  // renumbered) block numbers.
  BBSizes.insert(BBSizes.begin()+NewBB->getNumber(), 0);

  // Likewise for BBOffsets.
  BBOffsets.insert(BBOffsets.begin()+NewBB->getNumber(), 0);

  // Next, update WaterList.  Specifically, we need to add NewMBB as having
  // available water after it.
  std::vector<MachineBasicBlock*>::iterator IP =
    std::lower_bound(WaterList.begin(), WaterList.end(), NewBB,
                     CompareMBBNumbers);
  WaterList.insert(IP, NewBB);
}


/// Split the basic block containing MI into two blocks, which are joined by
/// an unconditional branch.  Update datastructures and renumber blocks to
/// account for this change and returns the newly created block.
MachineBasicBlock *ARMConstantIslands::SplitBlockBeforeInstr(MachineInstr *MI) {
  MachineBasicBlock *OrigBB = MI->getParent();
  MachineFunction &MF = *OrigBB->getParent();

  // Create a new MBB for the code after the OrigBB.
  MachineBasicBlock *NewBB =
    MF.CreateMachineBasicBlock(OrigBB->getBasicBlock());
  MachineFunction::iterator MBBI = OrigBB; ++MBBI;
  MF.insert(MBBI, NewBB);

  // Splice the instructions starting with MI over to NewBB.
  NewBB->splice(NewBB->end(), OrigBB, MI, OrigBB->end());

  // Add an unconditional branch from OrigBB to NewBB.
  // Note the new unconditional branch is not being recorded.
  // There doesn't seem to be meaningful DebugInfo available; this doesn't
  // correspond to anything in the source.
  unsigned Opc = isThumb ? (isThumb2 ? ARM::t2B : ARM::tB) : ARM::B;
  BuildMI(OrigBB, DebugLoc::getUnknownLoc(), TII->get(Opc)).addMBB(NewBB);
  NumSplit++;

  // Update the CFG.  All succs of OrigBB are now succs of NewBB.
  while (!OrigBB->succ_empty()) {
    MachineBasicBlock *Succ = *OrigBB->succ_begin();
    OrigBB->removeSuccessor(Succ);
    NewBB->addSuccessor(Succ);

    // This pass should be run after register allocation, so there should be no
    // PHI nodes to update.
    assert((Succ->empty() || Succ->begin()->getOpcode() != TargetInstrInfo::PHI)
           && "PHI nodes should be eliminated by now!");
  }

  // OrigBB branches to NewBB.
  OrigBB->addSuccessor(NewBB);

  // Update internal data structures to account for the newly inserted MBB.
  // This is almost the same as UpdateForInsertedWaterBlock, except that
  // the Water goes after OrigBB, not NewBB.
  MF.RenumberBlocks(NewBB);

  // Insert a size into BBSizes to align it properly with the (newly
  // renumbered) block numbers.
  BBSizes.insert(BBSizes.begin()+NewBB->getNumber(), 0);

  // Likewise for BBOffsets.
  BBOffsets.insert(BBOffsets.begin()+NewBB->getNumber(), 0);

  // Next, update WaterList.  Specifically, we need to add OrigMBB as having
  // available water after it (but not if it's already there, which happens
  // when splitting before a conditional branch that is followed by an
  // unconditional branch - in that case we want to insert NewBB).
  std::vector<MachineBasicBlock*>::iterator IP =
    std::lower_bound(WaterList.begin(), WaterList.end(), OrigBB,
                     CompareMBBNumbers);
  MachineBasicBlock* WaterBB = *IP;
  if (WaterBB == OrigBB)
    WaterList.insert(next(IP), NewBB);
  else
    WaterList.insert(IP, OrigBB);

  // Figure out how large the first NewMBB is.  (It cannot
  // contain a constpool_entry or tablejump.)
  unsigned NewBBSize = 0;
  for (MachineBasicBlock::iterator I = NewBB->begin(), E = NewBB->end();
       I != E; ++I)
    NewBBSize += TII->GetInstSizeInBytes(I);

  unsigned OrigBBI = OrigBB->getNumber();
  unsigned NewBBI = NewBB->getNumber();
  // Set the size of NewBB in BBSizes.
  BBSizes[NewBBI] = NewBBSize;

  // We removed instructions from UserMBB, subtract that off from its size.
  // Add 2 or 4 to the block to count the unconditional branch we added to it.
  int delta = isThumb1 ? 2 : 4;
  BBSizes[OrigBBI] -= NewBBSize - delta;

  // ...and adjust BBOffsets for NewBB accordingly.
  BBOffsets[NewBBI] = BBOffsets[OrigBBI] + BBSizes[OrigBBI];

  // All BBOffsets following these blocks must be modified.
  AdjustBBOffsetsAfter(NewBB, delta);

  return NewBB;
}

/// OffsetIsInRange - Checks whether UserOffset (the location of a constant pool
/// reference) is within MaxDisp of TrialOffset (a proposed location of a
/// constant pool entry).
bool ARMConstantIslands::OffsetIsInRange(unsigned UserOffset,
                                         unsigned TrialOffset, unsigned MaxDisp,
                                         bool NegativeOK, bool IsSoImm) {
  // On Thumb offsets==2 mod 4 are rounded down by the hardware for
  // purposes of the displacement computation; compensate for that here.
  // Effectively, the valid range of displacements is 2 bytes smaller for such
  // references.
  if (isThumb && UserOffset%4 !=0)
    UserOffset -= 2;
  // CPEs will be rounded up to a multiple of 4.
  if (isThumb && TrialOffset%4 != 0)
    TrialOffset += 2;

  if (UserOffset <= TrialOffset) {
    // User before the Trial.
    if (TrialOffset - UserOffset <= MaxDisp)
      return true;
    // FIXME: Make use full range of soimm values.
  } else if (NegativeOK) {
    if (UserOffset - TrialOffset <= MaxDisp)
      return true;
    // FIXME: Make use full range of soimm values.
  }
  return false;
}

/// WaterIsInRange - Returns true if a CPE placed after the specified
/// Water (a basic block) will be in range for the specific MI.

bool ARMConstantIslands::WaterIsInRange(unsigned UserOffset,
                                        MachineBasicBlock* Water, CPUser &U) {
  unsigned MaxDisp = U.MaxDisp;
  unsigned CPEOffset = BBOffsets[Water->getNumber()] +
                       BBSizes[Water->getNumber()];

  // If the CPE is to be inserted before the instruction, that will raise
  // the offset of the instruction.  (Currently applies only to ARM, so
  // no alignment compensation attempted here.)
  if (CPEOffset < UserOffset)
    UserOffset += U.CPEMI->getOperand(2).getImm();

  return OffsetIsInRange(UserOffset, CPEOffset, MaxDisp, U.NegOk, U.IsSoImm);
}

/// CPEIsInRange - Returns true if the distance between specific MI and
/// specific ConstPool entry instruction can fit in MI's displacement field.
bool ARMConstantIslands::CPEIsInRange(MachineInstr *MI, unsigned UserOffset,
                                      MachineInstr *CPEMI, unsigned MaxDisp,
                                      bool NegOk, bool DoDump) {
  unsigned CPEOffset  = GetOffsetOf(CPEMI);
  assert(CPEOffset%4 == 0 && "Misaligned CPE");

  if (DoDump) {
    DOUT << "User of CPE#" << CPEMI->getOperand(0).getImm()
         << " max delta=" << MaxDisp
         << " insn address=" << UserOffset
         << " CPE address=" << CPEOffset
         << " offset=" << int(CPEOffset-UserOffset) << "\t" << *MI;
  }

  return OffsetIsInRange(UserOffset, CPEOffset, MaxDisp, NegOk);
}

#ifndef NDEBUG
/// BBIsJumpedOver - Return true of the specified basic block's only predecessor
/// unconditionally branches to its only successor.
static bool BBIsJumpedOver(MachineBasicBlock *MBB) {
  if (MBB->pred_size() != 1 || MBB->succ_size() != 1)
    return false;

  MachineBasicBlock *Succ = *MBB->succ_begin();
  MachineBasicBlock *Pred = *MBB->pred_begin();
  MachineInstr *PredMI = &Pred->back();
  if (PredMI->getOpcode() == ARM::B || PredMI->getOpcode() == ARM::tB
      || PredMI->getOpcode() == ARM::t2B)
    return PredMI->getOperand(0).getMBB() == Succ;
  return false;
}
#endif // NDEBUG

void ARMConstantIslands::AdjustBBOffsetsAfter(MachineBasicBlock *BB,
                                              int delta) {
  MachineFunction::iterator MBBI = BB; MBBI = next(MBBI);
  for(unsigned i = BB->getNumber()+1, e = BB->getParent()->getNumBlockIDs();
      i < e; ++i) {
    BBOffsets[i] += delta;
    // If some existing blocks have padding, adjust the padding as needed, a
    // bit tricky.  delta can be negative so don't use % on that.
    if (!isThumb)
      continue;
    MachineBasicBlock *MBB = MBBI;
    if (!MBB->empty()) {
      // Constant pool entries require padding.
      if (MBB->begin()->getOpcode() == ARM::CONSTPOOL_ENTRY) {
        unsigned oldOffset = BBOffsets[i] - delta;
        if (oldOffset%4==0 && BBOffsets[i]%4!=0) {
          // add new padding
          BBSizes[i] += 2;
          delta += 2;
        } else if (oldOffset%4!=0 && BBOffsets[i]%4==0) {
          // remove existing padding
          BBSizes[i] -=2;
          delta -= 2;
        }
      }
      // Thumb1 jump tables require padding.  They should be at the end;
      // following unconditional branches are removed by AnalyzeBranch.
      MachineInstr *ThumbJTMI = prior(MBB->end());
      if (ThumbJTMI->getOpcode() == ARM::tBR_JTr) {
        unsigned newMIOffset = GetOffsetOf(ThumbJTMI);
        unsigned oldMIOffset = newMIOffset - delta;
        if (oldMIOffset%4 == 0 && newMIOffset%4 != 0) {
          // remove existing padding
          BBSizes[i] -= 2;
          delta -= 2;
        } else if (oldMIOffset%4 != 0 && newMIOffset%4 == 0) {
          // add new padding
          BBSizes[i] += 2;
          delta += 2;
        }
      }
      if (delta==0)
        return;
    }
    MBBI = next(MBBI);
  }
}

/// DecrementOldEntry - find the constant pool entry with index CPI
/// and instruction CPEMI, and decrement its refcount.  If the refcount
/// becomes 0 remove the entry and instruction.  Returns true if we removed
/// the entry, false if we didn't.

bool ARMConstantIslands::DecrementOldEntry(unsigned CPI, MachineInstr *CPEMI) {
  // Find the old entry. Eliminate it if it is no longer used.
  CPEntry *CPE = findConstPoolEntry(CPI, CPEMI);
  assert(CPE && "Unexpected!");
  if (--CPE->RefCount == 0) {
    RemoveDeadCPEMI(CPEMI);
    CPE->CPEMI = NULL;
    NumCPEs--;
    return true;
  }
  return false;
}

/// LookForCPEntryInRange - see if the currently referenced CPE is in range;
/// if not, see if an in-range clone of the CPE is in range, and if so,
/// change the data structures so the user references the clone.  Returns:
/// 0 = no existing entry found
/// 1 = entry found, and there were no code insertions or deletions
/// 2 = entry found, and there were code insertions or deletions
int ARMConstantIslands::LookForExistingCPEntry(CPUser& U, unsigned UserOffset)
{
  MachineInstr *UserMI = U.MI;
  MachineInstr *CPEMI  = U.CPEMI;

  // Check to see if the CPE is already in-range.
  if (CPEIsInRange(UserMI, UserOffset, CPEMI, U.MaxDisp, U.NegOk, true)) {
    DOUT << "In range\n";
    return 1;
  }

  // No.  Look for previously created clones of the CPE that are in range.
  unsigned CPI = CPEMI->getOperand(1).getIndex();
  std::vector<CPEntry> &CPEs = CPEntries[CPI];
  for (unsigned i = 0, e = CPEs.size(); i != e; ++i) {
    // We already tried this one
    if (CPEs[i].CPEMI == CPEMI)
      continue;
    // Removing CPEs can leave empty entries, skip
    if (CPEs[i].CPEMI == NULL)
      continue;
    if (CPEIsInRange(UserMI, UserOffset, CPEs[i].CPEMI, U.MaxDisp, U.NegOk)) {
      DOUT << "Replacing CPE#" << CPI << " with CPE#" << CPEs[i].CPI << "\n";
      // Point the CPUser node to the replacement
      U.CPEMI = CPEs[i].CPEMI;
      // Change the CPI in the instruction operand to refer to the clone.
      for (unsigned j = 0, e = UserMI->getNumOperands(); j != e; ++j)
        if (UserMI->getOperand(j).isCPI()) {
          UserMI->getOperand(j).setIndex(CPEs[i].CPI);
          break;
        }
      // Adjust the refcount of the clone...
      CPEs[i].RefCount++;
      // ...and the original.  If we didn't remove the old entry, none of the
      // addresses changed, so we don't need another pass.
      return DecrementOldEntry(CPI, CPEMI) ? 2 : 1;
    }
  }
  return 0;
}

/// getUnconditionalBrDisp - Returns the maximum displacement that can fit in
/// the specific unconditional branch instruction.
static inline unsigned getUnconditionalBrDisp(int Opc) {
  switch (Opc) {
  case ARM::tB:
    return ((1<<10)-1)*2;
  case ARM::t2B:
    return ((1<<23)-1)*2;
  default:
    break;
  }
  
  return ((1<<23)-1)*4;
}

/// AcceptWater - Small amount of common code factored out of the following.

MachineBasicBlock* ARMConstantIslands::AcceptWater(MachineBasicBlock *WaterBB,
                          std::vector<MachineBasicBlock*>::iterator IP) {
  DOUT << "found water in range\n";
  // Remove the original WaterList entry; we want subsequent
  // insertions in this vicinity to go after the one we're
  // about to insert.  This considerably reduces the number
  // of times we have to move the same CPE more than once.
  WaterList.erase(IP);
  // CPE goes before following block (NewMBB).
  return next(MachineFunction::iterator(WaterBB));
}

/// LookForWater - look for an existing entry in the WaterList in which
/// we can place the CPE referenced from U so it's within range of U's MI.
/// Returns true if found, false if not.  If it returns true, *NewMBB
/// is set to the WaterList entry.
/// For ARM, we prefer the water that's farthest away. For Thumb, prefer
/// water that will not introduce padding to water that will; within each
/// group, prefer the water that's farthest away.
bool ARMConstantIslands::LookForWater(CPUser &U, unsigned UserOffset,
                                      MachineBasicBlock** NewMBB) {
  std::vector<MachineBasicBlock*>::iterator IPThatWouldPad;
  MachineBasicBlock* WaterBBThatWouldPad = NULL;
  if (!WaterList.empty()) {
    for (std::vector<MachineBasicBlock*>::iterator IP = prior(WaterList.end()),
           B = WaterList.begin();; --IP) {
      MachineBasicBlock* WaterBB = *IP;
      if (WaterIsInRange(UserOffset, WaterBB, U)) {
        unsigned WBBId = WaterBB->getNumber();
        if (isThumb &&
            (BBOffsets[WBBId] + BBSizes[WBBId])%4 != 0) {
          // This is valid Water, but would introduce padding.  Remember
          // it in case we don't find any Water that doesn't do this.
          if (!WaterBBThatWouldPad) {
            WaterBBThatWouldPad = WaterBB;
            IPThatWouldPad = IP;
          }
        } else {
          *NewMBB = AcceptWater(WaterBB, IP);
          return true;
        }
      }
      if (IP == B)
        break;
    }
  }
  if (isThumb && WaterBBThatWouldPad) {
    *NewMBB = AcceptWater(WaterBBThatWouldPad, IPThatWouldPad);
    return true;
  }
  return false;
}

/// CreateNewWater - No existing WaterList entry will work for
/// CPUsers[CPUserIndex], so create a place to put the CPE.  The end of the
/// block is used if in range, and the conditional branch munged so control
/// flow is correct.  Otherwise the block is split to create a hole with an
/// unconditional branch around it.  In either case *NewMBB is set to a
/// block following which the new island can be inserted (the WaterList
/// is not adjusted).

void ARMConstantIslands::CreateNewWater(unsigned CPUserIndex,
                        unsigned UserOffset, MachineBasicBlock** NewMBB) {
  CPUser &U = CPUsers[CPUserIndex];
  MachineInstr *UserMI = U.MI;
  MachineInstr *CPEMI  = U.CPEMI;
  MachineBasicBlock *UserMBB = UserMI->getParent();
  unsigned OffsetOfNextBlock = BBOffsets[UserMBB->getNumber()] +
                               BBSizes[UserMBB->getNumber()];
  assert(OffsetOfNextBlock== BBOffsets[UserMBB->getNumber()+1]);

  // If the use is at the end of the block, or the end of the block
  // is within range, make new water there.  (The addition below is
  // for the unconditional branch we will be adding:  4 bytes on ARM + Thumb2,
  // 2 on Thumb1.  Possible Thumb1 alignment padding is allowed for
  // inside OffsetIsInRange.
  // If the block ends in an unconditional branch already, it is water,
  // and is known to be out of range, so we'll always be adding a branch.)
  if (&UserMBB->back() == UserMI ||
      OffsetIsInRange(UserOffset, OffsetOfNextBlock + (isThumb1 ? 2: 4),
                      U.MaxDisp, U.NegOk, U.IsSoImm)) {
    DOUT << "Split at end of block\n";
    if (&UserMBB->back() == UserMI)
      assert(BBHasFallthrough(UserMBB) && "Expected a fallthrough BB!");
    *NewMBB = next(MachineFunction::iterator(UserMBB));
    // Add an unconditional branch from UserMBB to fallthrough block.
    // Record it for branch lengthening; this new branch will not get out of
    // range, but if the preceding conditional branch is out of range, the
    // targets will be exchanged, and the altered branch may be out of
    // range, so the machinery has to know about it.
    int UncondBr = isThumb ? ((isThumb2) ? ARM::t2B : ARM::tB) : ARM::B;
    BuildMI(UserMBB, DebugLoc::getUnknownLoc(),
            TII->get(UncondBr)).addMBB(*NewMBB);
    unsigned MaxDisp = getUnconditionalBrDisp(UncondBr);
    ImmBranches.push_back(ImmBranch(&UserMBB->back(),
                          MaxDisp, false, UncondBr));
    int delta = isThumb1 ? 2 : 4;
    BBSizes[UserMBB->getNumber()] += delta;
    AdjustBBOffsetsAfter(UserMBB, delta);
  } else {
    // What a big block.  Find a place within the block to split it.
    // This is a little tricky on Thumb1 since instructions are 2 bytes
    // and constant pool entries are 4 bytes: if instruction I references
    // island CPE, and instruction I+1 references CPE', it will
    // not work well to put CPE as far forward as possible, since then
    // CPE' cannot immediately follow it (that location is 2 bytes
    // farther away from I+1 than CPE was from I) and we'd need to create
    // a new island.  So, we make a first guess, then walk through the
    // instructions between the one currently being looked at and the
    // possible insertion point, and make sure any other instructions
    // that reference CPEs will be able to use the same island area;
    // if not, we back up the insertion point.

    // The 4 in the following is for the unconditional branch we'll be
    // inserting (allows for long branch on Thumb1).  Alignment of the
    // island is handled inside OffsetIsInRange.
    unsigned BaseInsertOffset = UserOffset + U.MaxDisp -4;
    // This could point off the end of the block if we've already got
    // constant pool entries following this block; only the last one is
    // in the water list.  Back past any possible branches (allow for a
    // conditional and a maximally long unconditional).
    if (BaseInsertOffset >= BBOffsets[UserMBB->getNumber()+1])
      BaseInsertOffset = BBOffsets[UserMBB->getNumber()+1] -
                              (isThumb1 ? 6 : 8);
    unsigned EndInsertOffset = BaseInsertOffset +
           CPEMI->getOperand(2).getImm();
    MachineBasicBlock::iterator MI = UserMI;
    ++MI;
    unsigned CPUIndex = CPUserIndex+1;
    for (unsigned Offset = UserOffset+TII->GetInstSizeInBytes(UserMI);
         Offset < BaseInsertOffset;
         Offset += TII->GetInstSizeInBytes(MI),
            MI = next(MI)) {
      if (CPUIndex < CPUsers.size() && CPUsers[CPUIndex].MI == MI) {
        CPUser &U = CPUsers[CPUIndex];
        if (!OffsetIsInRange(Offset, EndInsertOffset,
                             U.MaxDisp, U.NegOk, U.IsSoImm)) {
          BaseInsertOffset -= (isThumb1 ? 2 : 4);
          EndInsertOffset  -= (isThumb1 ? 2 : 4);
        }
        // This is overly conservative, as we don't account for CPEMIs
        // being reused within the block, but it doesn't matter much.
        EndInsertOffset += CPUsers[CPUIndex].CPEMI->getOperand(2).getImm();
        CPUIndex++;
      }
    }
    DOUT << "Split in middle of big block\n";
    *NewMBB = SplitBlockBeforeInstr(prior(MI));
  }
}

/// HandleConstantPoolUser - Analyze the specified user, checking to see if it
/// is out-of-range.  If so, pick up the constant pool value and move it some
/// place in-range.  Return true if we changed any addresses (thus must run
/// another pass of branch lengthening), false otherwise.
bool ARMConstantIslands::HandleConstantPoolUser(MachineFunction &MF,
                                                unsigned CPUserIndex) {
  CPUser &U = CPUsers[CPUserIndex];
  MachineInstr *UserMI = U.MI;
  MachineInstr *CPEMI  = U.CPEMI;
  unsigned CPI = CPEMI->getOperand(1).getIndex();
  unsigned Size = CPEMI->getOperand(2).getImm();
  MachineBasicBlock *NewMBB;
  // Compute this only once, it's expensive.  The 4 or 8 is the value the
  // hardware keeps in the PC (2 insns ahead of the reference).
  unsigned UserOffset = GetOffsetOf(UserMI) + (isThumb ? 4 : 8);

  // See if the current entry is within range, or there is a clone of it
  // in range.
  int result = LookForExistingCPEntry(U, UserOffset);
  if (result==1) return false;
  else if (result==2) return true;

  // No existing clone of this CPE is within range.
  // We will be generating a new clone.  Get a UID for it.
  unsigned ID = AFI->createConstPoolEntryUId();

  // Look for water where we can place this CPE.  We look for the farthest one
  // away that will work.  Forward references only for now (although later
  // we might find some that are backwards).

  if (!LookForWater(U, UserOffset, &NewMBB)) {
    // No water found.
    DOUT << "No water found\n";
    CreateNewWater(CPUserIndex, UserOffset, &NewMBB);
  }

  // Okay, we know we can put an island before NewMBB now, do it!
  MachineBasicBlock *NewIsland = MF.CreateMachineBasicBlock();
  MF.insert(NewMBB, NewIsland);

  // Update internal data structures to account for the newly inserted MBB.
  UpdateForInsertedWaterBlock(NewIsland);

  // Decrement the old entry, and remove it if refcount becomes 0.
  DecrementOldEntry(CPI, CPEMI);

  // Now that we have an island to add the CPE to, clone the original CPE and
  // add it to the island.
  U.CPEMI = BuildMI(NewIsland, DebugLoc::getUnknownLoc(),
                    TII->get(ARM::CONSTPOOL_ENTRY))
                .addImm(ID).addConstantPoolIndex(CPI).addImm(Size);
  CPEntries[CPI].push_back(CPEntry(U.CPEMI, ID, 1));
  NumCPEs++;

  BBOffsets[NewIsland->getNumber()] = BBOffsets[NewMBB->getNumber()];
  // Compensate for .align 2 in thumb mode.
  if (isThumb && BBOffsets[NewIsland->getNumber()]%4 != 0)
    Size += 2;
  // Increase the size of the island block to account for the new entry.
  BBSizes[NewIsland->getNumber()] += Size;
  AdjustBBOffsetsAfter(NewIsland, Size);

  // Finally, change the CPI in the instruction operand to be ID.
  for (unsigned i = 0, e = UserMI->getNumOperands(); i != e; ++i)
    if (UserMI->getOperand(i).isCPI()) {
      UserMI->getOperand(i).setIndex(ID);
      break;
    }

  DOUT << "  Moved CPE to #" << ID << " CPI=" << CPI << "\t" << *UserMI;

  return true;
}

/// RemoveDeadCPEMI - Remove a dead constant pool entry instruction. Update
/// sizes and offsets of impacted basic blocks.
void ARMConstantIslands::RemoveDeadCPEMI(MachineInstr *CPEMI) {
  MachineBasicBlock *CPEBB = CPEMI->getParent();
  unsigned Size = CPEMI->getOperand(2).getImm();
  CPEMI->eraseFromParent();
  BBSizes[CPEBB->getNumber()] -= Size;
  // All succeeding offsets have the current size value added in, fix this.
  if (CPEBB->empty()) {
    // In thumb1 mode, the size of island may be padded by two to compensate for
    // the alignment requirement.  Then it will now be 2 when the block is
    // empty, so fix this.
    // All succeeding offsets have the current size value added in, fix this.
    if (BBSizes[CPEBB->getNumber()] != 0) {
      Size += BBSizes[CPEBB->getNumber()];
      BBSizes[CPEBB->getNumber()] = 0;
    }
  }
  AdjustBBOffsetsAfter(CPEBB, -Size);
  // An island has only one predecessor BB and one successor BB. Check if
  // this BB's predecessor jumps directly to this BB's successor. This
  // shouldn't happen currently.
  assert(!BBIsJumpedOver(CPEBB) && "How did this happen?");
  // FIXME: remove the empty blocks after all the work is done?
}

/// RemoveUnusedCPEntries - Remove constant pool entries whose refcounts
/// are zero.
bool ARMConstantIslands::RemoveUnusedCPEntries() {
  unsigned MadeChange = false;
  for (unsigned i = 0, e = CPEntries.size(); i != e; ++i) {
      std::vector<CPEntry> &CPEs = CPEntries[i];
      for (unsigned j = 0, ee = CPEs.size(); j != ee; ++j) {
        if (CPEs[j].RefCount == 0 && CPEs[j].CPEMI) {
          RemoveDeadCPEMI(CPEs[j].CPEMI);
          CPEs[j].CPEMI = NULL;
          MadeChange = true;
        }
      }
  }
  return MadeChange;
}

/// BBIsInRange - Returns true if the distance between specific MI and
/// specific BB can fit in MI's displacement field.
bool ARMConstantIslands::BBIsInRange(MachineInstr *MI,MachineBasicBlock *DestBB,
                                     unsigned MaxDisp) {
  unsigned PCAdj      = isThumb ? 4 : 8;
  unsigned BrOffset   = GetOffsetOf(MI) + PCAdj;
  unsigned DestOffset = BBOffsets[DestBB->getNumber()];

  DOUT << "Branch of destination BB#" << DestBB->getNumber()
       << " from BB#" << MI->getParent()->getNumber()
       << " max delta=" << MaxDisp
       << " from " << GetOffsetOf(MI) << " to " << DestOffset
       << " offset " << int(DestOffset-BrOffset) << "\t" << *MI;

  if (BrOffset <= DestOffset) {
    // Branch before the Dest.
    if (DestOffset-BrOffset <= MaxDisp)
      return true;
  } else {
    if (BrOffset-DestOffset <= MaxDisp)
      return true;
  }
  return false;
}

/// FixUpImmediateBr - Fix up an immediate branch whose destination is too far
/// away to fit in its displacement field.
bool ARMConstantIslands::FixUpImmediateBr(MachineFunction &MF, ImmBranch &Br) {
  MachineInstr *MI = Br.MI;
  MachineBasicBlock *DestBB = MI->getOperand(0).getMBB();

  // Check to see if the DestBB is already in-range.
  if (BBIsInRange(MI, DestBB, Br.MaxDisp))
    return false;

  if (!Br.isCond)
    return FixUpUnconditionalBr(MF, Br);
  return FixUpConditionalBr(MF, Br);
}

/// FixUpUnconditionalBr - Fix up an unconditional branch whose destination is
/// too far away to fit in its displacement field. If the LR register has been
/// spilled in the epilogue, then we can use BL to implement a far jump.
/// Otherwise, add an intermediate branch instruction to a branch.
bool
ARMConstantIslands::FixUpUnconditionalBr(MachineFunction &MF, ImmBranch &Br) {
  MachineInstr *MI = Br.MI;
  MachineBasicBlock *MBB = MI->getParent();
  assert(isThumb && !isThumb2 && "Expected a Thumb1 function!");

  // Use BL to implement far jump.
  Br.MaxDisp = (1 << 21) * 2;
  MI->setDesc(TII->get(ARM::tBfar));
  BBSizes[MBB->getNumber()] += 2;
  AdjustBBOffsetsAfter(MBB, 2);
  HasFarJump = true;
  NumUBrFixed++;

  DOUT << "  Changed B to long jump " << *MI;

  return true;
}

/// FixUpConditionalBr - Fix up a conditional branch whose destination is too
/// far away to fit in its displacement field. It is converted to an inverse
/// conditional branch + an unconditional branch to the destination.
bool
ARMConstantIslands::FixUpConditionalBr(MachineFunction &MF, ImmBranch &Br) {
  MachineInstr *MI = Br.MI;
  MachineBasicBlock *DestBB = MI->getOperand(0).getMBB();

  // Add an unconditional branch to the destination and invert the branch
  // condition to jump over it:
  // blt L1
  // =>
  // bge L2
  // b   L1
  // L2:
  ARMCC::CondCodes CC = (ARMCC::CondCodes)MI->getOperand(1).getImm();
  CC = ARMCC::getOppositeCondition(CC);
  unsigned CCReg = MI->getOperand(2).getReg();

  // If the branch is at the end of its MBB and that has a fall-through block,
  // direct the updated conditional branch to the fall-through block. Otherwise,
  // split the MBB before the next instruction.
  MachineBasicBlock *MBB = MI->getParent();
  MachineInstr *BMI = &MBB->back();
  bool NeedSplit = (BMI != MI) || !BBHasFallthrough(MBB);

  NumCBrFixed++;
  if (BMI != MI) {
    if (next(MachineBasicBlock::iterator(MI)) == prior(MBB->end()) &&
        BMI->getOpcode() == Br.UncondBr) {
      // Last MI in the BB is an unconditional branch. Can we simply invert the
      // condition and swap destinations:
      // beq L1
      // b   L2
      // =>
      // bne L2
      // b   L1
      MachineBasicBlock *NewDest = BMI->getOperand(0).getMBB();
      if (BBIsInRange(MI, NewDest, Br.MaxDisp)) {
        DOUT << "  Invert Bcc condition and swap its destination with " << *BMI;
        BMI->getOperand(0).setMBB(DestBB);
        MI->getOperand(0).setMBB(NewDest);
        MI->getOperand(1).setImm(CC);
        return true;
      }
    }
  }

  if (NeedSplit) {
    SplitBlockBeforeInstr(MI);
    // No need for the branch to the next block. We're adding an unconditional
    // branch to the destination.
    int delta = TII->GetInstSizeInBytes(&MBB->back());
    BBSizes[MBB->getNumber()] -= delta;
    MachineBasicBlock* SplitBB = next(MachineFunction::iterator(MBB));
    AdjustBBOffsetsAfter(SplitBB, -delta);
    MBB->back().eraseFromParent();
    // BBOffsets[SplitBB] is wrong temporarily, fixed below
  }
  MachineBasicBlock *NextBB = next(MachineFunction::iterator(MBB));

  DOUT << "  Insert B to BB#" << DestBB->getNumber()
       << " also invert condition and change dest. to BB#"
       << NextBB->getNumber() << "\n";

  // Insert a new conditional branch and a new unconditional branch.
  // Also update the ImmBranch as well as adding a new entry for the new branch.
  BuildMI(MBB, DebugLoc::getUnknownLoc(),
          TII->get(MI->getOpcode()))
    .addMBB(NextBB).addImm(CC).addReg(CCReg);
  Br.MI = &MBB->back();
  BBSizes[MBB->getNumber()] += TII->GetInstSizeInBytes(&MBB->back());
  BuildMI(MBB, DebugLoc::getUnknownLoc(), TII->get(Br.UncondBr)).addMBB(DestBB);
  BBSizes[MBB->getNumber()] += TII->GetInstSizeInBytes(&MBB->back());
  unsigned MaxDisp = getUnconditionalBrDisp(Br.UncondBr);
  ImmBranches.push_back(ImmBranch(&MBB->back(), MaxDisp, false, Br.UncondBr));

  // Remove the old conditional branch.  It may or may not still be in MBB.
  BBSizes[MI->getParent()->getNumber()] -= TII->GetInstSizeInBytes(MI);
  MI->eraseFromParent();

  // The net size change is an addition of one unconditional branch.
  int delta = TII->GetInstSizeInBytes(&MBB->back());
  AdjustBBOffsetsAfter(MBB, delta);
  return true;
}

/// UndoLRSpillRestore - Remove Thumb push / pop instructions that only spills
/// LR / restores LR to pc.
bool ARMConstantIslands::UndoLRSpillRestore() {
  bool MadeChange = false;
  for (unsigned i = 0, e = PushPopMIs.size(); i != e; ++i) {
    MachineInstr *MI = PushPopMIs[i];
    if (MI->getOpcode() == ARM::tPOP_RET &&
        MI->getOperand(0).getReg() == ARM::PC &&
        MI->getNumExplicitOperands() == 1) {
      BuildMI(MI->getParent(), MI->getDebugLoc(), TII->get(ARM::tBX_RET));
      MI->eraseFromParent();
      MadeChange = true;
    }
  }
  return MadeChange;
}

bool ARMConstantIslands::OptimizeThumb2JumpTables(MachineFunction &MF) {
  bool MadeChange = false;

  // FIXME: After the tables are shrunk, can we get rid some of the
  // constantpool tables?
  const MachineJumpTableInfo *MJTI = MF.getJumpTableInfo();
  const std::vector<MachineJumpTableEntry> &JT = MJTI->getJumpTables();
  for (unsigned i = 0, e = T2JumpTables.size(); i != e; ++i) {
    MachineInstr *MI = T2JumpTables[i];
    const TargetInstrDesc &TID = MI->getDesc();
    unsigned NumOps = TID.getNumOperands();
    unsigned JTOpIdx = NumOps - (TID.isPredicable() ? 3 : 2);
    MachineOperand JTOP = MI->getOperand(JTOpIdx);
    unsigned JTI = JTOP.getIndex();
    assert(JTI < JT.size());

    bool ByteOk = true;
    bool HalfWordOk = true;
    unsigned JTOffset = GetOffsetOf(MI) + 4;
    const std::vector<MachineBasicBlock*> &JTBBs = JT[JTI].MBBs;
    for (unsigned j = 0, ee = JTBBs.size(); j != ee; ++j) {
      MachineBasicBlock *MBB = JTBBs[j];
      unsigned DstOffset = BBOffsets[MBB->getNumber()];
      // Negative offset is not ok. FIXME: We should change BB layout to make
      // sure all the branches are forward.
      if (ByteOk && (DstOffset - JTOffset) > ((1<<8)-1)*2)
        ByteOk = false;
      unsigned TBHLimit = ((1<<16)-1)*2;
      if (STI->isTargetDarwin())
        TBHLimit >>= 1;  // FIXME: Work around an assembler bug.
      if (HalfWordOk && (DstOffset - JTOffset) > TBHLimit)
        HalfWordOk = false;
      if (!ByteOk && !HalfWordOk)
        break;
    }

    if (ByteOk || HalfWordOk) {
      MachineBasicBlock *MBB = MI->getParent();
      unsigned BaseReg = MI->getOperand(0).getReg();
      bool BaseRegKill = MI->getOperand(0).isKill();
      if (!BaseRegKill)
        continue;
      unsigned IdxReg = MI->getOperand(1).getReg();
      bool IdxRegKill = MI->getOperand(1).isKill();
      MachineBasicBlock::iterator PrevI = MI;
      if (PrevI == MBB->begin())
        continue;

      MachineInstr *AddrMI = --PrevI;
      bool OptOk = true;
      // Examine the instruction that calculate the jumptable entry address.
      // If it's not the one just before the t2BR_JT, we won't delete it, then
      // it's not worth doing the optimization.
      for (unsigned k = 0, eee = AddrMI->getNumOperands(); k != eee; ++k) {
        const MachineOperand &MO = AddrMI->getOperand(k);
        if (!MO.isReg() || !MO.getReg())
          continue;
        if (MO.isDef() && MO.getReg() != BaseReg) {
          OptOk = false;
          break;
        }
        if (MO.isUse() && !MO.isKill() && MO.getReg() != IdxReg) {
          OptOk = false;
          break;
        }
      }
      if (!OptOk)
        continue;

      // The previous instruction should be a t2LEApcrelJT, we want to delete
      // it as well.
      MachineInstr *LeaMI = --PrevI;
      if (LeaMI->getOpcode() != ARM::t2LEApcrelJT ||
          LeaMI->getOperand(0).getReg() != BaseReg)
        OptOk = false;

      if (!OptOk)
        continue;

      unsigned Opc = ByteOk ? ARM::t2TBB : ARM::t2TBH;
      MachineInstr *NewJTMI = BuildMI(MBB, MI->getDebugLoc(), TII->get(Opc))
        .addReg(IdxReg, getKillRegState(IdxRegKill))
        .addJumpTableIndex(JTI, JTOP.getTargetFlags())
        .addImm(MI->getOperand(JTOpIdx+1).getImm());
      // FIXME: Insert an "ALIGN" instruction to ensure the next instruction
      // is 2-byte aligned. For now, asm printer will fix it up.
      unsigned NewSize = TII->GetInstSizeInBytes(NewJTMI);
      unsigned OrigSize = TII->GetInstSizeInBytes(AddrMI);
      OrigSize += TII->GetInstSizeInBytes(LeaMI);
      OrigSize += TII->GetInstSizeInBytes(MI);

      AddrMI->eraseFromParent();
      LeaMI->eraseFromParent();
      MI->eraseFromParent();

      int delta = OrigSize - NewSize;
      BBSizes[MBB->getNumber()] -= delta;
      AdjustBBOffsetsAfter(MBB, -delta);

      ++NumTBs;
      MadeChange = true;
    }
  }

  return MadeChange;
}
