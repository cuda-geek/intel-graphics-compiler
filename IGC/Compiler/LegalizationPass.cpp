/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "AdaptorCommon/ImplicitArgs.hpp"
#include "Compiler/LegalizationPass.hpp"
#include "Compiler/CodeGenPublic.h"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/MetaDataApi/MetaDataApi.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Transforms/Utils/Local.h>
#include "common/LLVMWarningsPop.hpp"

#include "GenISAIntrinsics/GenIntrinsicInst.h"

using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;
using namespace IGC::IGCMD;

namespace IGC {

bool expandFDIVInstructions(llvm::Function& F);

} // namespace IGC

static cl::opt<bool> PreserveNan(
    "preserve-nan", cl::init(false), cl::Hidden,
    cl::desc("Preserve NAN (default false)"));

// Register pass to igc-opt
#define PASS_FLAG "igc-legalization"
#define PASS_DESCRIPTION "VISA Legalizer"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(Legalization, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(Legalization, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char Legalization::ID = 0;

Legalization::Legalization(bool preserveNan)
    : FunctionPass(ID), m_preserveNan(preserveNan),
      m_preserveNanCheck(m_preserveNan), m_DL(0)
{
    initializeLegalizationPass(*PassRegistry::getPassRegistry());
}

bool Legalization::runOnFunction(Function &F)
{
    m_ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    MetaDataUtils *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    auto MD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    if (pMdUtils->findFunctionsInfoItem(&F) == pMdUtils->end_FunctionsInfo())
    {
        return false;
    }
    if (MD->compOpt.FiniteMathOnly)
    {
        m_preserveNan = false;
        // Do not preserve nan but honor nan checks.
        m_preserveNanCheck = true;
    }
    llvm::IRBuilder<> builder(F.getContext());
    m_builder = &builder;
    // Emit pass doesn't support constant expressions, therefore we do not expect to run into them in this pass    
    for (auto I = inst_begin(F), E = inst_end(F); I != E; ++I)
    {
        for (auto OI = I->op_begin(), OE = I->op_end(); OI != OE; ++OI)
        {
            assert(!isa<ConstantExpr>(OI) && "Function must not contain constant expressions");
        }
    }

    // Create a unique return instruction for this funciton if necessary.
    unifyReturnInsts(F);

    m_DL = &F.getParent()->getDataLayout();
    // recalculate this field
    m_ctx->m_instrTypes.numInsts = 0;

    visit(F);

    for(auto I : m_instructionsToRemove)
    {
        I->eraseFromParent();
    }

    m_instructionsToRemove.clear();

    // Legalize fdiv if any
    if (!m_ctx->platform.hasFDIV())
        expandFDIVInstructions(F);
    return true;
}

void Legalization::unifyReturnInsts(llvm::Function &F)
{
    // Adapted from llvm::UnifyFunctionExitNodes.cpp
    //
    // Loop over all of the blocks in a function, tracking all of the blocks
    // that return.
    SmallVector<BasicBlock *, 16> ReturningBlocks;
    for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
        if (isa<ReturnInst>(I->getTerminator()))
            ReturningBlocks.push_back(&(*I));

    // Now handle return blocks.
    if (ReturningBlocks.size() <= 1)
        return;

    // Otherwise, we need to insert a new basic block into the function,
    // add a PHI nodes (if the function returns values), and convert
    // all of the return instructions into unconditional branches.
    BasicBlock *NewRetBlock =
        BasicBlock::Create(F.getContext(), "UnifiedReturnBlock", &F);

    PHINode *PN = nullptr;
    if (F.getReturnType()->isVoidTy())
        ReturnInst::Create(F.getContext(), nullptr, NewRetBlock);
    else
    {
        // If the function doesn't return void... add a PHI node to the block...
        PN = PHINode::Create(F.getReturnType(), ReturningBlocks.size(),
            "UnifiedRetVal");
        NewRetBlock->getInstList().push_back(PN);
        ReturnInst::Create(F.getContext(), PN, NewRetBlock);
    }

    // Loop over all of the blocks, replacing the return instruction with an
    // unconditional branch.
    for (auto BB : ReturningBlocks)
    {
        // Add an incoming element to the PHI node for every return instruction that
        // is merging into this new block...
        if (PN)
            PN->addIncoming(BB->getTerminator()->getOperand(0), BB);

        BB->getInstList().pop_back(); // Remove the return inst.
        BranchInst::Create(NewRetBlock, BB);
    }
}

void Legalization::markToRemove(llvm::Instruction* I)
{
    m_instructionsToRemove.insert(I);

    // Let go all the operands, so we can later remove also their definitions.
    I->dropAllReferences();
}

void Legalization::visitInstruction(llvm::Instruction &I)
{
    if (!llvm::isa<llvm::DbgInfoIntrinsic>(&I))
        m_ctx->m_instrTypes.numInsts++;
}

void Legalization::visitBinaryOperator(llvm::BinaryOperator &I)
{
    if(I.getOpcode() == Instruction::FRem)
    {
        Function* floorFunc =
            Intrinsic::getDeclaration(m_ctx->getModule(), Intrinsic::floor, I.getType());
        m_builder->SetInsertPoint(&I);
        Value* a = I.getOperand(0);
        Value* b = I.getOperand(1);
        Value* mulab = m_builder->CreateFMul(a, b);
        Value* sign = m_builder->CreateFCmpOGE(mulab, m_builder->CreateFNeg(mulab));
        Value* sel = m_builder->CreateSelect(sign, b, m_builder->CreateFNeg(b));
        Value* selInv = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.f), sel);
        Value* div = m_builder->CreateFMul(a, selInv);
        Value* floordiv = m_builder->CreateCall(floorFunc, div);
        Value* frc = m_builder->CreateFSub(div, floordiv);
        Value* result = m_builder->CreateFMul(frc, sel);
        I.replaceAllUsesWith(result);
        I.eraseFromParent();
    }
	else if (I.getOpcode() == Instruction::And || I.getOpcode() == Instruction::Or)
	{
		// convert (!a and !b) to !(a or b)
		// convert (!a or !b) to !(a and b)
		// then remove the negate by flipping all the uses (select or branch)
		Value *src0 = I.getOperand(0);
		Value *src1 = I.getOperand(1);
		if (llvm::BinaryOperator::isNot(src0) &&
			llvm::BinaryOperator::isNot(src1) &&
			src0->hasOneUse() && src1->hasOneUse()) {
			// check all uses are select or branch
			bool flippable = true;
			for (auto U = I.user_begin(), E = I.user_end(); U != E; ++U)
			{
				if (!isa<SelectInst>(*U) && !isa<BranchInst>(*U))
				{
					flippable = false;
					break;
				}
                // check select i1 with I not used as condition
                if (isa<SelectInst>(*U) && U->getOperand(0) != &I)
                {
                    flippable = false;
                    break;
                }
			}
			if (flippable)
			{
				Value *invert;
				if (I.getOpcode() == Instruction::And)
				{
					invert =
						llvm::BinaryOperator::CreateOr(
							cast<llvm::Instruction>(src0)->getOperand(0),
							cast<llvm::Instruction>(src1)->getOperand(0),
							"",
							&I);
				}
				else
				{
					invert =
						llvm::BinaryOperator::CreateAnd(
							cast<llvm::Instruction>(src0)->getOperand(0),
							cast<llvm::Instruction>(src1)->getOperand(0),
							"",
							&I);
				}
                while (!I.user_empty())
				{
                    auto U = I.user_begin();
					if (SelectInst* s = dyn_cast<SelectInst>(*U))
					{
                        Value* trueValue = s->getTrueValue();
                        Value* falseValue = s->getFalseValue();
                        s->setOperand(1, falseValue);
                        s->setOperand(2, trueValue);
                        s->setOperand(0, invert);
					}
					else if (BranchInst* br = dyn_cast<BranchInst>(*U))
					{
						assert(br->isConditional());
						br->swapSuccessors();
						br->setCondition(invert);
					}
				}
				I.eraseFromParent();
				cast<llvm::Instruction>(src0)->eraseFromParent();
				cast<llvm::Instruction>(src1)->eraseFromParent();
			}
		}
	}
    m_ctx->m_instrTypes.numInsts++;
}

void Legalization::visitCallInst(llvm::CallInst &I)
{
    m_ctx->m_instrTypes.numInsts++;
    if (!m_ctx->platform.supportSamplerFp16Input())
    {
        // promote FP16 sample_xxx to FP32 sample_xxx
        if (llvm::isa<SampleIntrinsic>(&I)      ||
            llvm::isa<SamplerGatherIntrinsic>(&I))
        {
            if (I.getOperand(0)->getType()->isHalfTy())
            {
                PromoteFp16ToFp32OnGenSampleCall(I);
            }
        }
    }
}

// Match and legalize the following patterns out of GVN:
//
// (1)
// %23 = bitcast <3 x half> %assembled.vect35 to i48
// %trunc = trunc i48 %23 to i16
// %bitcast = bitcast i16 %trunc to half
//
// (2)
// %23 = bitcast <3 x half> %assembled.vect35 to i48
// %27 = lshr i48 %23, 16
// %trunc = trunc i48 %27 to i16
// %bitcast = bitcast i16 %28 to half
//
// into
//
// (1-legalized)
// %30 = extract <3 x half> %assembled.vect35, i32 0
// <replace all uses of %bitcast by %30>
//
// (2-legalized)
// 31 = extract <3 x half> %assembled.vect35, i32 1
// <replace all uses of %bitcast by %31>
//
// Case 3:
//
// %158 = bitcast <4 x float> %130 to i128
// %trunc = trunc i128 %158 to i96
// %bitcast = bitcast i96 %trunc to <3 x float>
// %scalar92 = extractelement <3 x float> %bitcast, i32 0
// %scalar93 = extractelement <3 x float> %bitcast, i32 1
// %scalar94 = extractelement <3 x float> %bitcast, i32 2
//
// into
//
// (3-legalized)
// %scalar92_0 = extractelement <4 x float> %130, i32 0
// %scalar93_1 = extractelement <4 x float> %130, i32 1
// %scalar94_2 = extractelement <4 x float> %130, i32 2
// <replace all uses of %scalar9{2,3,4}>
//
// Case 4:
//
// (1)
// %24 = bitcast <4 x i32> %22 to i128
// %29 = trunc i128 %24 to i8
//
// (2)
// %24 = bitcast <4 x i32> %22 to i128
// %28 = lshr i128 %24, 8
// %29 = trunc i128 %28 to i8
//
// into
//
// (1-legalized)
// %24 = bitcast <4 x i32> %22 to <16 x i8>
// %28 = extractelement <16 x i8> %24 i32 0
//
// (2-legalized)
// %24 = bitcast <4 x i32> %22 to <16 x i8>
// %28 = extractelement <16 x i8> %24 i32 1
//

static bool
LegalizeGVNBitCastPattern(IRBuilder<> *Builder, const DataLayout *DL,
                          BitCastInst &I,
                          std::unordered_set<Instruction *> *m_instructionsToRemove)
{
    IntegerType *DstTy = dyn_cast<IntegerType>(I.getType());
    VectorType *SrcTy = dyn_cast<VectorType>(I.getOperand(0)->getType());
    if (!DstTy || !SrcTy || DL->isLegalInteger(DstTy->getBitWidth()))
    {
        return false;
    }

    Type *EltTy = SrcTy->getVectorElementType();
    auto match1 = [=](Value *V, BinaryOperator *&BO, TruncInst *&TI,
                      BitCastInst *&BI, int &Index)
    {
        // The leading instruction is optional.
        BO = dyn_cast<BinaryOperator>(V);
        if (BO)
        {
            if (BO->getOpcode() != Instruction::LShr || !BO->hasOneUse())
                return false;

            // The shift amount shall be a constant.
            Value *BOp1 = BO->getOperand(1);
            auto CI = dyn_cast<ConstantInt>(BOp1);
            if (!CI)
                return false;

            // The shift amount shall be a multiple of base element.
            uint64_t ShAmt = CI->getZExtValue();
            if (ShAmt % EltTy->getPrimitiveSizeInBits() != 0)
                return false;

            // Compute the index of the element to be extracted.
            Index = int_cast<int>(ShAmt / EltTy->getPrimitiveSizeInBits());
        }

        // The second instruction is *not* optional.
        if (BO)
            TI = dyn_cast<TruncInst>(BO->user_back());
        else
            TI = dyn_cast<TruncInst>(V);

        if (!TI || !TI->hasOneUse())
            return false;

        // Optionally, followed by a bitcast.
        // Assign null to BI if this is not ending with a bitcast.
        BI = dyn_cast<BitCastInst>(TI->user_back());

        // This gurantees all uses of BI could be replaced by the source.
        if (BI && BI->getType() != EltTy)
            return false;
        else if (TI->getType()->getPrimitiveSizeInBits() !=
                 EltTy->getPrimitiveSizeInBits())
            return false;

        return true;
    };

    // %158 = bitcast <4 x float> %130 to i128
    // %trunc = trunc i128 %158 to i96                          // V, TI
    // %bitcast = bitcast i96 %trunc to <3 x float>             // BI
    // %scalar92 = extractelement <3 x float> %bitcast, i32 0   // EEI[0]
    // %scalar93 = extractelement <3 x float> %bitcast, i32 1   // EEI[1]
    // %scalar94 = extractelement <3 x float> %bitcast, i32 2   // EEI[2]
    //
    // Match the above pattern and initialize TI, BI, EEIs
    auto match2 = [=](Value *V, TruncInst *&TI, BitCastInst *&BI,
                      SmallVectorImpl<ExtractElementInst *> &EEIs)
    {
        TI = dyn_cast<TruncInst>(V);
        if (!TI || !TI->hasOneUse())
            return false;

        BI = dyn_cast<BitCastInst>(TI->user_back());

        // Only valid for vector type.
        if (!BI || !BI->getType()->isVectorTy())
            return false;

        // All uses must be EEI.
        for (auto U : BI->users())
        {
            auto EEI = dyn_cast<ExtractElementInst>(U);
            if (!EEI)
                return false;
            EEIs.push_back(EEI);
        }
        return true;
    };

    auto match3 = [=](Value *V, BinaryOperator *&BO, TruncInst *&TI, int &Index)
    {
        // The lshr instruction is optional.
        BO = dyn_cast<BinaryOperator>(V);
        if (BO && (BO->getOpcode() != Instruction::LShr || !BO->hasOneUse()))
            return false;

        // The trunc instruction is *not* optional.
        if (BO)
            TI = dyn_cast<TruncInst>(BO->user_back());
        else
            TI = dyn_cast<TruncInst>(V);

        if (!TI)
            return false;

        int srcSize = int_cast<int>(TI->getOperand(0)->getType()->getPrimitiveSizeInBits());
        int dstSize = int_cast<int>(TI->getType()->getPrimitiveSizeInBits());
        if(srcSize % dstSize != 0)
            return false;

        if (BO)
        {
            // The shift amount shall be a constant.
            Value *BOp1 = BO->getOperand(1);
            auto CI = dyn_cast<ConstantInt>(BOp1);
            if (!CI)
                return false;

            // The shift amount shall be a multiple of base element.
            uint64_t ShAmt = CI->getZExtValue();
            uint64_t ElSize = TI->getType()->getPrimitiveSizeInBits();
            if (ShAmt % ElSize != 0)
                return false;

            // Compute the index of the element to be extracted.
            Index = int_cast<int>(ShAmt / ElSize);
        }

        return true;
    };

    for (auto U : I.users())
    {
        // Case 1 and 2 and 4.
        BinaryOperator *BO = nullptr; // the lshr instruction, optional
        TruncInst *TI = nullptr; // not optional
        BitCastInst *BI = nullptr; // optional
        int Index = 0; // the vector element index.

        // Case 3 only.
        SmallVector<ExtractElementInst *, 8> EEIs;

        if (match1(U, BO, TI, BI, Index))
        {
            if (BI)
                Builder->SetInsertPoint(BI);
            else
                Builder->SetInsertPoint(TI);

            Value *V = Builder->CreateExtractElement(
                I.getOperand(0),
                ConstantInt::get(Type::getInt32Ty(I.getContext()), Index));

            if (BI)
            {
                assert(BI->getType() == EltTy);

                // BO, TI, and BI are dead.
                BI->replaceAllUsesWith(V);
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(BI);
                }

                TI->replaceAllUsesWith(UndefValue::get(TI->getType()));
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(TI);
                }
            }
            else
            {
                assert(TI->getType()->getPrimitiveSizeInBits() ==
                       EltTy->getPrimitiveSizeInBits());
                if (V->getType() != TI->getType())
                    V = Builder->CreateBitCast(V, TI->getType());

                // BO and TI are dead.
                TI->replaceAllUsesWith(V);
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(TI);
                }
            }

            if (BO)
            {
                BO->replaceAllUsesWith(UndefValue::get(BO->getType()));
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(BO);
                }
            }
        }
        else if (match2(U, TI, BI, EEIs))
        {
            for (auto EEI : EEIs)
            {
                Builder->SetInsertPoint(EEI);
                // The index operand remains the same since there is no
                // shift on the wide integer source.
                Value *V = Builder->CreateExtractElement(
                    I.getOperand(0), EEI->getIndexOperand());
                if(V->getType() != EEI->getType())
                {
                    V = Builder->CreateBitCast(V, EEI->getType());
                }
                EEI->replaceAllUsesWith(V);
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(EEI);
                }
            }
        }
        else if (match3(U, BO, TI, Index))
        {
            // Example:
            // %24 = bitcast <4 x i32> %22 to i128
            // %28 = lshr i128 %24, 8
            // %29 = trunc i128 %28 to i8
            Type* castType = TI->getType();
            int srcSize = int_cast<int>(TI->getOperand(0)->getType()->getPrimitiveSizeInBits());
            int dstSize = int_cast<int>(castType->getPrimitiveSizeInBits());

            // vecSize is 128/8 = 16 in above example
            assert(srcSize % dstSize == 0);
            uint vecSize = srcSize / dstSize;

            Builder->SetInsertPoint(TI);
            Value *BC = Builder->CreateBitCast(I.getOperand(0), VectorType::get(castType, vecSize));
            Value *EE = Builder->CreateExtractElement(BC, ConstantInt::get(Type::getInt32Ty(I.getContext()), Index));

            // BO and TI are dead
            TI->replaceAllUsesWith(EE);
            if (m_instructionsToRemove)
            {
                m_instructionsToRemove->insert(TI);
            }
            if (BO)
            {
                BO->replaceAllUsesWith(UndefValue::get(BO->getType()));
                if (m_instructionsToRemove)
                {
                    m_instructionsToRemove->insert(BO);
                }
            }
        }
    }

    return true;
}

void Legalization::visitBitCastInst(llvm::BitCastInst &I)
{
    m_ctx->m_instrTypes.numInsts++;
    // This is the pass that folds 2x Float into a Double replacing the bitcast intruction
    if(ConstantDataVector* vec = dyn_cast<ConstantDataVector>(I.getOperand(0)))
    {
        unsigned int nbElement = vec->getNumElements();
        //nbElement == 2 implies the bitcast instruction has a 2X Float src and we are checking if the destination is of Type Double
        if(nbElement == 2 && I.getType()->isDoubleTy() && vec->getElementType()->isFloatTy())
        {
            //Extracting LSB form srcVec
            ConstantFP* srcLSB = cast<ConstantFP>(vec->getElementAsConstant(0));
            uint64_t LSB = srcLSB->getValueAPF().bitcastToAPInt().getZExtValue();

            //Extracting MSB form srcVec
            ConstantFP* srcMSB = cast<ConstantFP>(vec->getElementAsConstant(1));
            uint64_t MSB = srcMSB->getValueAPF().bitcastToAPInt().getZExtValue();

            //Replacing the bitcast instruction with 2x float to a emit a double value
            uint64_t rslt = ((MSB << 32)|LSB);
            // Yes, this is a hack. double result = static_cast<double>(rslt) didn't generate the correct double equivalent for rslt
            double result = *(double *)&rslt;
            ConstantFP* newVec = cast<ConstantFP>(ConstantFP::get(Type::getDoubleTy(I.getContext()), result));

            I.replaceAllUsesWith(newVec);
            I.eraseFromParent();
            return;
        }
    }

    // GVN creates patterns that use large integer or illegal types (i128, i256,
    // i48 etc.) from vectors of smaller types. The cases we see can be easily
    // modified to use extracts.
    if (LegalizeGVNBitCastPattern(m_builder, m_DL, I, &m_instructionsToRemove))
    {
        if (I.use_empty())
        {
            m_instructionsToRemove.insert(&I);
        }
        return;
    }

    [&]() {
        // Example:
        // %y = trunc i64 %x to i48
        // %z = bitcast i48 %y to <3 x half>
        // ==>
        // %y = bitcast i64 %x to <4 x half>
        // %z = shufflevector <4 x half> %y, <4 x half> undef, <3 x i32> <i32 0, i32 1, i32 2> 

        auto *pZ = &I;

        if (!pZ->getSrcTy()->isIntegerTy(48) &&
            !pZ->getSrcTy()->isIntegerTy(24))
            return;

        if (!isa<VectorType>(pZ->getDestTy()))
            return;

        if (!isa<TruncInst>(pZ->getOperand(0)))
            return;

        auto *pVecTy = cast<VectorType>(pZ->getDestTy());
        if (pVecTy->getNumElements() != 3)
            return;

        auto *pEltTy = pVecTy->getElementType();

        auto *pY = cast<TruncInst>(pZ->getOperand(0));
        auto *pX = pY->getOperand(0);
        
        if (!pX->getType()->isIntegerTy(64) &&
            !pX->getType()->isIntegerTy(32))
            return;

        uint numElt = pX->getType()->getPrimitiveSizeInBits() / pEltTy->getPrimitiveSizeInBits();
        auto *pBCType = VectorType::get(pEltTy, numElt);

        SmallVector<uint32_t, 4> maskVals;
        for (uint i = 0; i < pVecTy->getNumElements(); i++)
        {
            maskVals.push_back(i);
        }
        auto *pMask = ConstantDataVector::get(I.getContext(), maskVals);

        auto *pNewY = BitCastInst::CreateBitOrPointerCast(pX, pBCType, "", pZ);
        auto *pNewZ = new ShuffleVectorInst(pNewY, UndefValue::get(pBCType), pMask);
        pNewZ->insertAfter(pNewY);

        pZ->replaceAllUsesWith(pNewZ);
        pZ->eraseFromParent();

        if (pY->use_empty())
        {
            pY->eraseFromParent();
        }

        // Legalize the shuffle vector that we just generated.
        visitShuffleVectorInst(*pNewZ);
    }();
}

void Legalization::visitSelectInst(SelectInst& I)
{
    m_ctx->m_instrTypes.numInsts++;
    if (I.getType()->isIntegerTy(1))
    {
        llvm::Value* pCond = I.getOperand(0);
        llvm::Value* pSrc0 = I.getOperand(1);
        llvm::Value* pSrc1 = I.getOperand(2);
        LLVMContext& context = I.getContext();
        llvm::Instruction* pSrc0ZExt = 
            llvm::CastInst::CreateZExtOrBitCast(pSrc0, Type::getInt32Ty(context), "", &I);

        llvm::Instruction* pSrc1ZExt = 
            llvm::CastInst::CreateZExtOrBitCast(pSrc1, Type::getInt32Ty(context), "", &I);

        // Create a new Select instruction
        llvm::SelectInst* pNewSel = llvm::SelectInst::Create(pCond, pSrc0ZExt, pSrc1ZExt, "", &I);
        llvm::CastInst* pTruncInst = 
            llvm::CastInst::CreateTruncOrBitCast(pNewSel, Type::getInt1Ty(context), "", &I);

        I.replaceAllUsesWith(pTruncInst);
        I.eraseFromParent();
    }
    else if (I.getType()->isDoubleTy() &&
        (IGC_IS_FLAG_ENABLED(ForceDPEmulation) ||
         !m_ctx->platform.supportFP64()))
    {
        // Split double select to i32 select.

        Value* lo[2];
        Value* hi[2];
        Type* intTy = Type::getInt32Ty(I.getContext());
        VectorType* vec2Ty = VectorType::get(intTy, 2);
        Constant* Zero = ConstantInt::get(intTy, 0);
        Constant* One = ConstantInt::get(intTy, 1);
        m_builder->SetInsertPoint(&I);
        for (int i=0; i < 2; ++i)
        {
            Value* twoi32 = m_builder->CreateBitCast(I.getOperand(i+1), vec2Ty);
            lo[i] = m_builder->CreateExtractElement(twoi32, Zero);
            hi[i] = m_builder->CreateExtractElement(twoi32, One);
        }        

        Value* new_lo = m_builder->CreateSelect(I.getCondition(), lo[0], lo[1]);
        Value* new_hi = m_builder->CreateSelect(I.getCondition(), hi[0], hi[1]);
 
        Value* newVal = m_builder->CreateInsertElement(UndefValue::get(vec2Ty), new_lo, Zero);
        newVal = m_builder->CreateInsertElement(newVal, new_hi, One);
        newVal = m_builder->CreateBitCast(newVal, I.getType());

        I.replaceAllUsesWith(newVal);
        I.eraseFromParent();
    }
    else if(I.getType()->isVectorTy())
    {
        unsigned int vecSize = I.getType()->getVectorNumElements();
        Value* newVec = UndefValue::get(I.getType());
        m_builder->SetInsertPoint(&I);
        for(unsigned int i = 0; i<vecSize; i++)
        {
            Value* idx = m_builder->getInt32(i);
            Value* condVal = I.getCondition();
            if (condVal->getType()->isVectorTy()) {
                condVal = m_builder->CreateExtractElement(condVal, idx);
            }
            Value* trueVal = m_builder->CreateExtractElement(I.getTrueValue(), idx);
            Value* falseVal = m_builder->CreateExtractElement(I.getFalseValue(), idx);
            Value* sel = m_builder->CreateSelect(condVal, trueVal, falseVal);
            newVec = m_builder->CreateInsertElement(newVec, sel, idx);
        }
        I.replaceAllUsesWith(newVec);
        I.eraseFromParent();
    }
}

void Legalization::visitPHINode(PHINode& phi)
{
    m_ctx->m_instrTypes.numInsts++;
    // break down phi of i1
    LLVMContext& context = phi.getContext();
    if(phi.getType()->isIntegerTy(1))
    {
        unsigned int nbOperand = phi.getNumOperands();
        Type* newType = Type::getInt32Ty(context);
        PHINode* newPhi = PHINode::Create(newType, nbOperand, "", &phi);
        for(unsigned int i = 0; i<nbOperand; i++)
        {
            Value* source = phi.getOperand(i);
            Instruction* terminator = phi.getIncomingBlock(i)->getTerminator();
            m_builder->SetInsertPoint(terminator);
            Value *newSource = m_builder->CreateSExt(source, newType);
            newPhi->addIncoming(newSource, phi.getIncomingBlock(i));
        }
        Value* boolean = 
            CmpInst::Create(
                Instruction::ICmp, CmpInst::ICMP_NE, newPhi, ConstantInt::get(newType, 0), "", phi.getParent()->getFirstNonPHI());
        phi.replaceAllUsesWith(boolean);
        phi.eraseFromParent();
    }

}

static Value *GetMaskedValue(IRBuilder<> *IRB, bool Signed, Value *Src, Type *Ty) {
    IntegerType *SrcITy = dyn_cast<IntegerType>(Src->getType());
    IntegerType *ITy = dyn_cast<IntegerType>(Ty);

    assert(SrcITy && ITy && SrcITy->getBitWidth() > ITy->getBitWidth() &&
            "The source integer must be wider than the target integer.");

    if (!Signed) // For unsigned value, just mask off non-significant bits.
        return IRB->CreateAnd(Src, ITy->getBitMask());

    auto ShAmt = SrcITy->getBitWidth() - ITy->getBitWidth();
    return IRB->CreateAShr(IRB->CreateShl(Src, ShAmt), ShAmt);
}

void Legalization::visitICmpInst(ICmpInst &IC)
{   
    Value* Op0 = IC.getOperand(0);
    Value* Op1 = IC.getOperand(1);
    Type *Ty = Op0->getType();
    if (Ty->isIntegerTy(1)) { 
        Value* operand0_i8 = CastInst::CreateIntegerCast(Op0, Type::getInt8Ty(IC.getContext()), IC.isSigned(), "", &IC);
        Value* operand1_i8 = CastInst::CreateIntegerCast(Op1, Type::getInt8Ty(IC.getContext()), IC.isSigned(), "", &IC);
        IRBuilder<> m_build(&IC);
        Value* new_IC = m_build.CreateICmp(IC.getPredicate(), operand0_i8, operand1_i8, "");
        IC.replaceAllUsesWith(new_IC);
        IC.eraseFromParent();
    }

    if (Ty->isIntegerTy() && m_DL->isIllegalInteger(Ty->getIntegerBitWidth()) && isa<TruncInst>(Op0) && isa<ConstantInt>(Op1)) {
        // Legalize
        //
        // (icmp (trunc i32 to i28) C)
        //
        // TODO: It should be straightforward to supoprt other cases.
        //
        TruncInst *TI = cast<TruncInst>(Op0);
        Value *Src = TI->getOperand(0);
        Type *SrcTy = Src->getType();

        m_builder->SetInsertPoint(&IC);

        Value *NOp0 = GetMaskedValue(m_builder, IC.isSigned(), Src, Ty);
        Value *NOp1 = IC.isSigned() ? m_builder->CreateSExt(Op1, SrcTy)
                                    : m_builder->CreateZExt(Op1, SrcTy);
        Value *NCmp = m_builder->CreateICmp(IC.getPredicate(), NOp0, NOp1);
        IC.replaceAllUsesWith(NCmp);
        IC.eraseFromParent();
    }
}

Value* Legalization::addFCmpWithORD(FCmpInst &FC)
{
    m_builder->SetInsertPoint(&FC);

    //Are both sources Not NaN's ? 
    // %c = fcmp ord %a %b
    // =>
    // %1 = fcmp oeq %a %a
    // %2 = fcmp oeq %b %b
    // %c = and %1 %2

    Value *Op0 = FC.getOperand(0);
    Value *Op1 = FC.getOperand(1);

    return m_builder->CreateAnd(m_builder->CreateFCmpOEQ(Op0, Op0),
                                m_builder->CreateFCmpOEQ(Op1, Op1));
}

Value* Legalization::addFCmpWithUNO(FCmpInst &FC)
{
    //Is any of the sources NaN's
    // %c = fcmp uno %a %b
    // =>
    // %1 = fcmp une %a %a
    // %2 = fcmp une %b %b
    // %c = or %1 %2
    Value *src0 = FC.getOperand(0);
    Value *src1 = FC.getOperand(1);

    if (isa<ConstantFP>(src0))
        std::swap(src0, src1);

    Value* c0 =
        FCmpInst::Create(Instruction::FCmp, CmpInst::FCMP_UNE, src0, src0, "", &FC);

    if (ConstantFP* CFP = dyn_cast<ConstantFP>(src1))
    {
        if (!CFP->isNaN())
            return c0;
    }

    Value* c1 = 
        FCmpInst::Create(Instruction::FCmp, CmpInst::FCMP_UNE, src1, src1, "", &FC);

    Value* isAnySourceUnordered =
        llvm::BinaryOperator::CreateOr(c0, c1, "", &FC);

    return isAnySourceUnordered;
}

void Legalization::visitFCmpInstUndorderedPredicate(FCmpInst &FC)
{
    Value *result = nullptr;
    switch ( FC.getPredicate() )
    {
    case CmpInst::FCMP_ORD:
        result = addFCmpWithORD(FC);
        break;
    case CmpInst::FCMP_UNO:        
        result = addFCmpWithUNO(FC);
        break;
    case CmpInst::FCMP_ONE:
        {
            // %c = fcmp one %a %b
            // =>
            // %1 = fcmp ord %a %b
            // %2 = fcmp une %a %b
            // %c = and %1 %2 
            Value* sourcesOrdered = addFCmpWithORD(FC);
            Value* fcmpNotEqual   = 
                FCmpInst::Create(
                    Instruction::FCmp, 
                    FCmpInst::FCMP_UNE,
                    FC.getOperand(0),
                    FC.getOperand(1),
                    "",
                    &FC);
            result = 
                llvm::BinaryOperator::CreateAnd(
                    sourcesOrdered,
                    fcmpNotEqual,
                    "",
                    &FC);
        }
        break;
    case CmpInst::FCMP_UEQ:
        {
            // %c = fcmp ueq %a %b
            // =>
            // %1 = fcmp uno %a %b
            // %2 = fcmp oeq %a %b
            // %c = or %1 %2
            Value* sourcesUnordered = addFCmpWithUNO(FC);
            Value* fcmpEqual   = 
                FCmpInst::Create(
                    Instruction::FCmp, 
                    FCmpInst::FCMP_OEQ,
                    FC.getOperand(0),
                    FC.getOperand(1),
                    "",
                    &FC);
            result = 
                llvm::BinaryOperator::CreateOr(
                    sourcesUnordered,
                    fcmpEqual,
                    "",
                    &FC);
        }
        break;
    case CmpInst::FCMP_UGE: 
    case CmpInst::FCMP_UGT:
    case CmpInst::FCMP_ULE:
    case CmpInst::FCMP_ULT:
        {
            //To handle Unordered predicates, convert them to inverted ordered 
            //and than not the result
            // e.g. %c = fcmp uge %a %b
            //      =>
            //      %1 = fcmp olt %a %b
            //      %c = not %1
            Value* invertedOrderedInst = 
                FCmpInst::Create(
                    Instruction::FCmp, 
                    FCmpInst::getInversePredicate(FC.getPredicate()),
                    FC.getOperand(0),
                    FC.getOperand(1),
                    "",
                    &FC);

            while (!FC.user_empty())
            {
                auto I = FC.user_begin();
                if (SelectInst* s = dyn_cast<SelectInst>(*I))
                {
                    // check whether FC is condition
                    if (s->getOperand(0) == &FC)
                    {
                        Value* trueValue = s->getTrueValue();
                        Value* falseValue = s->getFalseValue();
                        s->setOperand(1, falseValue);
                        s->setOperand(2, trueValue);
                        s->setOperand(0, invertedOrderedInst);
                    }
                    else
                    {
                        break;
                    }
                }
                else if (BranchInst* br = dyn_cast<BranchInst>(*I))
                {
                    assert(br->isConditional());
                    br->swapSuccessors();
                    br->setCondition(invertedOrderedInst);
                }
                else
                {
                    break;
                }
            }

            if(!FC.use_empty())
            {
                result = llvm::BinaryOperator::CreateNot(invertedOrderedInst,"", &FC);
            }
            else
            {
                FC.eraseFromParent();
            }

        }
        break;
    default:
        break;
    }

    if (result)
    {
        FC.replaceAllUsesWith(result);
        FC.eraseFromParent();
    }
}

// See comments for m_preserveNanCheck.
static bool isNanCheck(FCmpInst &FC)
{
    Value *Op1 = FC.getOperand(1);
    if (FC.getPredicate() == CmpInst::FCMP_UNO)
    {
        auto CFP = dyn_cast<ConstantFP>(Op1);
        return CFP && CFP->isZero();
    }
    else if (FC.getPredicate() == CmpInst::FCMP_UNE)
    {
        Value *Op0 = FC.getOperand(0);
        return Op0 == Op1;
    }

    return false;
}

void Legalization::visitFCmpInst(FCmpInst &FC)
{
    m_ctx->m_instrTypes.numInsts++;
    // Handling NaN's for FCmp.
    if (FCmpInst::isUnordered(FC.getPredicate()) ||
        FC.getPredicate() == CmpInst::FCMP_ORD ||
        FC.getPredicate() == CmpInst::FCMP_ONE)
    {
        if ((m_preserveNan || PreserveNan) && !FC.isFast())
        {
            visitFCmpInstUndorderedPredicate(FC);
        }
        else if (m_preserveNanCheck && isNanCheck(FC))
        {
            visitFCmpInstUndorderedPredicate(FC);
        }
        else
        {
            visitFCmpInstUndorderedFlushNan(FC);
        }
    }
}

CmpInst::Predicate getOrderedPredicate(CmpInst::Predicate pred)
{
    switch(pred)
    {
    case CmpInst::FCMP_UEQ: return CmpInst::FCMP_OEQ;
    case CmpInst::FCMP_UNE: return CmpInst::FCMP_ONE;
    case CmpInst::FCMP_UGT: return CmpInst::FCMP_OGT;
    case CmpInst::FCMP_ULT: return CmpInst::FCMP_OLT;
    case CmpInst::FCMP_UGE: return CmpInst::FCMP_OGE;
    case CmpInst::FCMP_ULE: return CmpInst::FCMP_OLE;
    default:
        assert(0 && "wrong predicate");
        break;
    }
    return pred;
}

// legalize compare predicate ignoring Nan
void Legalization::visitFCmpInstUndorderedFlushNan(FCmpInst & FC)
{
    Value *result = nullptr;
    switch ( FC.getPredicate() )
    {
    case CmpInst::FCMP_ORD:
        result = ConstantInt::getTrue(FC.getType());
        break;
    case CmpInst::FCMP_UNO:        
        result = ConstantInt::getFalse(FC.getType());
        break;
    case CmpInst::FCMP_ONE:
         result = FCmpInst::Create(
                Instruction::FCmp, 
                FCmpInst::FCMP_UNE,
                FC.getOperand(0),
                FC.getOperand(1),
                "",
                &FC);
        break;
    case CmpInst::FCMP_UEQ:
        result = FCmpInst::Create(
                Instruction::FCmp, 
                FCmpInst::FCMP_OEQ,
                FC.getOperand(0),
                FC.getOperand(1),
                "",
                &FC);
        break;
    case CmpInst::FCMP_UGE: 
    case CmpInst::FCMP_UGT:
    case CmpInst::FCMP_ULE:
    case CmpInst::FCMP_ULT:
        result = FCmpInst::Create(
                Instruction::FCmp, 
                getOrderedPredicate(FC.getPredicate()),
                FC.getOperand(0),
                FC.getOperand(1),
                "",
                &FC);
        break;
    default:
        break;
    }

    if (result)
    {
        FC.replaceAllUsesWith(result);
        FC.eraseFromParent();
    }
}

void Legalization::visitStoreInst(StoreInst &I)
{
    m_ctx->m_instrTypes.numInsts++;
    if (m_instructionsToRemove.find(&I) != m_instructionsToRemove.end()) return;

    if(ConstantDataVector* vec = dyn_cast<ConstantDataVector>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(vec->getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getElementAsConstant(i);
            if(!isa<UndefValue>(cst))
            {
                newVec = InsertElementInst::Create(
                    newVec,
                    cst,
                    ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                    "",
                    &I);
            }
        }
        
        IGC::cloneStore(&I, newVec, I.getPointerOperand());
        I.eraseFromParent();
    }
    else if (ConstantVector* vec = dyn_cast<ConstantVector>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(vec->getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getOperand(i);
            if(!isa<UndefValue>(cst))
            {
                newVec = InsertElementInst::Create(
                    newVec,
                    cst,
                    ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                    "",
                    &I);
            }
        }
        
        IGC::cloneStore(&I, newVec, I.getPointerOperand());
        I.eraseFromParent();
    }
    else if (ConstantAggregateZero* vec = dyn_cast<ConstantAggregateZero>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(vec->getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getElementValue(i);
            if(!isa<UndefValue>(cst))
            {
                newVec = InsertElementInst::Create(
                    newVec,
                    cst,
                    ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                    "",
                    &I);
            }
        }
        
        IGC::cloneStore(&I, newVec, I.getPointerOperand());
        I.eraseFromParent();
    }
    else if (I.getOperand(0)->getType()->isIntegerTy(1))
    {
        m_builder->SetInsertPoint(&I);
        Value *newVal = m_builder->CreateZExt(I.getOperand(0), m_builder->getInt8Ty());

        PointerType *ptrTy = cast<PointerType>(I.getPointerOperand()->getType());
        unsigned addressSpace = ptrTy->getAddressSpace();
        PointerType *I8PtrTy = m_builder->getInt8PtrTy(addressSpace);
        Value *I8PtrOp = m_builder->CreateBitCast(I.getPointerOperand(), I8PtrTy);

        IGC::cloneStore(&I, newVal, I8PtrOp);
        I.eraseFromParent();
    }
    else if (I.getOperand(0)->getType()->isIntegerTy())
    {
        m_builder->SetInsertPoint(&I);

        unsigned srcWidth = I.getOperand(0)->getType()->getScalarSizeInBits();
        if (m_DL->isLegalInteger(srcWidth)) // nothing to legalize
            return;

        // Find largest legal int size to break into vectors
        unsigned intSize = 0;
        for(unsigned i = m_DL->getLargestLegalIntTypeSizeInBits(); i >= 8; i >>= 1)
        {
            if (srcWidth % i == 0)
            {
                intSize = i;
                break;
            }
        }
        if (intSize == 0) // unaligned sizes not supported
            return;

        Type* legalTy = VectorType::get(Type::getIntNTy(I.getContext(), intSize), srcWidth / intSize);
        Value* storeVal = BitCastInst::Create(Instruction::BitCast, I.getOperand(0), legalTy, "", &I);
        Value* storePtr = I.getPointerOperand();

        assert(storePtr->getType()->getPointerElementType()->isIntegerTy(srcWidth));

        PointerType* ptrTy = PointerType::get(legalTy, storePtr->getType()->getPointerAddressSpace());
        IntToPtrInst* intToPtr = dyn_cast<IntToPtrInst>(storePtr);

        if (intToPtr)
        {
            // Direct cast int to the legal type
            storePtr = IntToPtrInst::Create(Instruction::CastOps::IntToPtr, intToPtr->getOperand(0), ptrTy, "", &I);
        }
        else
        {
            storePtr = BitCastInst::CreatePointerCast(storePtr, ptrTy, "", &I);
        }
        IGC::cloneStore(&I, storeVal, storePtr);
        I.eraseFromParent();

        if (intToPtr && intToPtr->getNumUses() == 0)
        {
            intToPtr->eraseFromParent();
        }
    }
}

void Legalization::visitLoadInst(LoadInst &I)
{
    if (I.getType()->isIntegerTy(1))
    {
        m_builder->SetInsertPoint(&I);
        PointerType *ptrTy = cast<PointerType>(I.getPointerOperand()->getType());
        unsigned addressSpace = ptrTy->getAddressSpace();
        PointerType *I8PtrTy = m_builder->getInt8PtrTy(addressSpace);
        Value *I8PtrOp = m_builder->CreateBitCast(I.getPointerOperand(), I8PtrTy);

        LoadInst* pNewLoadInst = IGC::cloneLoad(&I, I8PtrOp);
        Value *newVal = m_builder->CreateTrunc(pNewLoadInst, I.getType());
        I.replaceAllUsesWith(newVal);
    }
}

void Legalization::RecursivelyPromoteInsertElementUses(Value *I, Value *packedVec)
{
    if (InsertElementInst *IEinst = dyn_cast<InsertElementInst>(I))
    {
        m_builder->SetInsertPoint(IEinst);

        Value* bitVal = m_builder->CreateZExt(IEinst->getOperand(1), m_builder->getInt8Ty());
        bitVal = m_builder->CreateShl(bitVal, m_builder->CreateTrunc(IEinst->getOperand(2), m_builder->getInt8Ty()));
        packedVec = m_builder->CreateOr(packedVec, bitVal);

        // We can modify user list of current instruction in RecursivelyPromoteInsertElementsUses
        // by removing users, so we need to cache users not to invalidate iterators.
        SmallVector<Value*, 8> users(I->users());
        for (auto user : users)
        {
            RecursivelyPromoteInsertElementUses(user, packedVec);
        }

        // After recursively promoting all the subsequent values in the def-use chain to something else
        // than i1, this particular use should have no further uses.
        if (IEinst->getNumUses() == 0)
        {
            markToRemove(IEinst);
        }
    }
    else if (ExtractElementInst* EEinst = dyn_cast<ExtractElementInst>(I))
    {
        m_builder->SetInsertPoint(EEinst);
        Value* newVal = m_builder->CreateAShr(packedVec, m_builder->CreateTrunc(EEinst->getOperand(1), m_builder->getInt8Ty()));
        newVal = m_builder->CreateAnd(newVal, m_builder->getInt8(1));

        for (Value::user_iterator useI = I->user_begin(), useE = I->user_end(); useI != useE; ++useI)
        {
            CastInst * castI = dyn_cast<CastInst>(*useI);
            if (castI &&
                castI->getOpcode() == Instruction::SExt &&
                castI->getSrcTy()->isIntegerTy(1) &&
                castI->getDestTy()->isIntegerTy(32))
            {
                newVal = m_builder->CreateSExt(newVal, m_builder->getInt32Ty());
                castI->replaceAllUsesWith(newVal);
            }
            else
            {
                llvm::Instruction* pSrc1ZExt =
                    llvm::CastInst::CreateTruncOrBitCast(newVal, Type::getInt1Ty(I->getContext()), "", EEinst);
                I->replaceAllUsesWith(pSrc1ZExt);
            }
        }

        // At this point we replaced all uses of extractelement uses (sic!) with extracted bit.
        // EEinst should have no uses left.
        assert(EEinst->getNumUses() == 0);
        markToRemove(EEinst);
    }
    else if (StoreInst *SI = dyn_cast<StoreInst>(I))
    {
        m_builder->SetInsertPoint(SI);

        PointerType *ptrTy = cast<PointerType>(SI->getPointerOperand()->getType());
        unsigned addressSpace = ptrTy->getAddressSpace();
        PointerType *I8PtrTy = m_builder->getInt8PtrTy(addressSpace);
        Value *I8PtrOp = m_builder->CreateBitCast(SI->getPointerOperand(), I8PtrTy);

        IGC::cloneStore(SI, packedVec, I8PtrOp);
        markToRemove(SI);
    }
}

void Legalization::visitInsertElementInst(InsertElementInst &I)
{
    m_ctx->m_instrTypes.numInsts++;

    if (m_instructionsToRemove.find(&I) != m_instructionsToRemove.end()) return;

    if(ConstantDataVector* vec = dyn_cast<ConstantDataVector>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(vec->getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getElementAsConstant(i);
            if(!isa<UndefValue>(cst))
            {
                newVec = InsertElementInst::Create(
                    newVec, 
                    cst,
                    ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                    "",
                    &I);
            }
        }
        newVec = InsertElementInst::Create(newVec, I.getOperand(1), I.getOperand(2), "", &I);
        I.replaceAllUsesWith(newVec);
    }
    else if(ConstantVector* vec = dyn_cast<ConstantVector>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(I.getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getOperand(i);
            if(!isa<UndefValue>(cst))
            {
                newVec = InsertElementInst::Create(
                    newVec, 
                    cst,
                    ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                    "",
                    &I);
            }
        }
        newVec = InsertElementInst::Create(newVec, I.getOperand(1), I.getOperand(2), "", &I);
        I.replaceAllUsesWith(newVec);
    }
    else if(ConstantAggregateZero* vec = dyn_cast<ConstantAggregateZero>(I.getOperand(0)))
    {
        Value* newVec = UndefValue::get(I.getType());
        unsigned int nbElement = vec->getType()->getVectorNumElements();
        for(unsigned int i = 0; i<nbElement; i++)
        {
            Constant* cst = vec->getElementValue(i);
            newVec = InsertElementInst::Create(
                newVec, 
                cst,
                ConstantInt::get(Type::getInt32Ty(I.getContext()), i),
                "",
                &I);
        }
        newVec = InsertElementInst::Create(newVec, I.getOperand(1), I.getOperand(2), "", &I);
        I.replaceAllUsesWith(newVec);
    }
    else if (I.getOperand(1)->getType()->isIntegerTy(1))
    {
        Value* vecOperand = I.getOperand(0);

        // Assumption here is, that we're legalizing chain of insertelements that fills single vector and
        // ends with extract element or store to memory. Given this, first insertelement should come with
        // undef vector as its source. If that's not true, it would mean we have some other instructions
        // generating our source vector that we should legalize, yet we don't know how.
        assert(vecOperand == UndefValue::get(VectorType::get(m_builder->getInt1Ty(), I.getOperand(0)->getType()->getVectorNumElements())));

        // We'll be converting insertelements to bitinserts to i8 value. To keep this reasonably simple
        // let's assume we can pack whole vector to i8:
        assert(vecOperand->getType()->getVectorNumElements() <= 8);
        
        Value* packedVec = m_builder->getInt8(0);

        RecursivelyPromoteInsertElementUses(&I, packedVec);
    }
}

void Legalization::visitShuffleVectorInst(ShuffleVectorInst &I)
{
    m_ctx->m_instrTypes.numInsts++;
    // Replace the shuffle with a series of inserts.
    // If the original vector is a constant, just use the scalar constant,
    // otherwise extract from the original vector.
    
    VectorType* resType = cast<VectorType>(I.getType());
    Value* newVec = UndefValue::get(resType);
    Value* src0 = I.getOperand(0);
    Value* src1 = I.getOperand(1);
    // The mask is guaranteed by the LLVM IR spec to be constant
    Constant* mask = cast<Constant>(I.getOperand(2));

    for(unsigned int dstIndex = 0; dstIndex < resType->getNumElements(); ++dstIndex)
    {
        // The mask value can be either an integer or undef. 
        // If it's undef, do nothing.
        // Otherwise, create an insert with the appropriate value.
        ConstantInt* index = dyn_cast<ConstantInt>(mask->getAggregateElement(dstIndex));
        if (index)
        {
            int indexVal = int_cast<int>(index->getZExtValue());

            // The two inputs are guaranteed to be of the same type
            VectorType* inType = cast<VectorType>(src0->getType());
            int inCount = int_cast<int>(inType->getNumElements());

            Value* srcVector = nullptr;
            int srcIndex = 0;
            if (indexVal < inCount)
            {
                srcVector = src0;
                srcIndex = indexVal;
            }
            else
            {
                srcVector = src1;
                srcIndex = indexVal - inCount;
            }

            // If the source is a constant vector (undef counts) just get the scalar
            // constant and insert that. Otherwise, add an extract from the appropriate
            // index.
            Value* srcVal = nullptr;
            Constant* constSrc = dyn_cast<Constant>(srcVector);
            if (constSrc)
            {
                srcVal = constSrc->getAggregateElement(dstIndex);
            }
            else
            {
                // Try to find the original inserted value.
                srcVal = findInsert(srcVector, srcIndex);

                //If we couldn't find it, just create a new extract.
                if (!srcVal)
                {
                    srcVal = ExtractElementInst::Create(srcVector, 
                                ConstantInt::get(index->getType(), srcIndex),
                                "",
                                &I);
                }
            }

            newVec = InsertElementInst::Create(newVec, 
                        srcVal,
                        ConstantInt::get(index->getType(), dstIndex),
                        "",
                        &I);
        }
    }

    I.replaceAllUsesWith(newVec);
    I.eraseFromParent();
}

llvm::Value* Legalization::findInsert(llvm::Value* vector, unsigned int index)
{
    // If the vector was constructed by a chain of inserts, 
    // walk up the chain until we find the correct value.
    InsertElementInst* IE = dyn_cast<InsertElementInst>(vector);
    while (IE)
    {
        ConstantInt* indexOp = dyn_cast<ConstantInt>(IE->getOperand(2));
        // There was a non-constant index, so all bets are off
        if (!indexOp)
          return nullptr;

        uint insertIndex = static_cast<uint>(indexOp->getZExtValue());
        if (insertIndex == index)
          return IE->getOperand(1);

        IE = dyn_cast<InsertElementInst>(IE->getOperand(0));
    }
    
    // Couldn't find an insert, so the index did not change from the initial
    // value of the chain.
    return nullptr;
}

Value* Cast(Value* val, Type* type, Instruction* insertBefore)
{
    Value* newVal = nullptr;
    if(type->isIntegerTy())
    {
        newVal = CastInst::CreateIntegerCast(val, type, false, "", insertBefore);
    }
    else if(type->isFloatingPointTy())
    {
        newVal = CastInst::CreateFPCast(val, type, "", insertBefore);
    }
    else
    {
        assert(0 && "unexpected type");
    }
    return newVal;
}

void Legalization::RecursivelyChangePointerType(Instruction* oldPtr, Instruction* newPtr)
{
    for(auto II = oldPtr->user_begin(), IE = oldPtr->user_end(); II != IE; ++II)
    {
        Value* newVal = nullptr;
        if(GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(*II))
        {
            SmallVector<Value*, 8> Idx(gep->idx_begin(), gep->idx_end());
            GetElementPtrInst* newGep = GetElementPtrInst::Create(nullptr, newPtr,Idx, "", gep);
            RecursivelyChangePointerType(gep, newGep);
        }
        else if(LoadInst* load = dyn_cast<LoadInst>(*II))
        {
            Instruction* newLoad = IGC::cloneLoad(load, newPtr);
            newVal = Cast(newLoad, load->getType(), load->getNextNode());
            load->replaceAllUsesWith(newVal);
        }
        else if(StoreInst* store = dyn_cast<StoreInst>(*II))
        {
            Value* StoredValue = store->getValueOperand();
            Value* newData = Cast(StoredValue, newPtr->getType()->getPointerElementType(), store);
            IGC::cloneStore(store, newData, newPtr);
        }
        else if(CastInst* cast = dyn_cast<CastInst>(*II))
        {
            Value* newCast = CastInst::CreatePointerCast(newPtr, cast->getType(), "", cast);
            cast->replaceAllUsesWith(newCast);
        }
        // We cannot delete any instructions as the visitor
        m_instructionsToRemove.insert(cast<Instruction>(*II));
    }
}

Type* Legalization::LegalStructAllocaType(Type* type) const {
    // Recursively legalize the struct type
    StructType *StTy = cast<StructType>(type);
    SmallVector<Type*, 8> Elems;
    bool IsIllegal = false;
    for (auto I = StTy->element_begin(), IE = StTy->element_end(); I != IE; ++I)
    {
        Type* LegalTy = LegalAllocaType(*I);
        Elems.push_back(LegalTy);
        IsIllegal = IsIllegal || LegalTy != *I;
    }
    if (IsIllegal)
    {
        type = StructType::get(type->getContext(), Elems);
    }
    return type;
}

Type* Legalization::LegalAllocaType(Type* type) const
{
    Type* legalType = type;
    switch(type->getTypeID())
    {
    case Type::IntegerTyID:
        if(type->isIntegerTy(1))
        {
            unsigned int size = int_cast<unsigned int>(m_DL->getTypeAllocSizeInBits(type));
            legalType = Type::getIntNTy(type->getContext(), size);
        }
        break;
    case Type::ArrayTyID:
        legalType = ArrayType::get(
            LegalAllocaType(type->getSequentialElementType()), 
            type->getArrayNumElements());
        break;
    case Type::VectorTyID:
        legalType = VectorType::get(
            LegalAllocaType(type->getSequentialElementType()), 
            type->getVectorNumElements());
        break;
    case Type::StructTyID:
        return LegalStructAllocaType(type);
    case Type::HalfTyID:
    case Type::FloatTyID:
    case Type::DoubleTyID:
    case Type::PointerTyID:
        break;
    default:
        assert(0 && "Alloca of unsupported type");
        break;
    }
    return legalType;
}

void Legalization::visitAlloca(AllocaInst& I)
{
    m_ctx->m_instrTypes.numInsts++;
    Type* type = I.getAllocatedType();
    Type* legalAllocaType = LegalAllocaType(type);
    if(type != legalAllocaType)
    {
        // Remaining alloca of i1 need to be promoted
        AllocaInst* newAlloca = new AllocaInst(legalAllocaType, 0, "", &I);
        RecursivelyChangePointerType(&I, newAlloca);
        m_instructionsToRemove.insert(&I);
    }
}

void Legalization::visitIntrinsicInst(llvm::IntrinsicInst &I)
{
    m_ctx->m_instrTypes.numInsts++;
    switch (I.getIntrinsicID())
    {
        case Intrinsic::uadd_with_overflow:
        {
            Value* src0 = I.getArgOperand(0);
            Value* src1 = I.getArgOperand(1);
            Value* res = BinaryOperator::Create(Instruction::Add, src0, src1, "", &I);
            // Unsigned a + b overflows iff a + b < a (for an unsigned comparison)
            Value* isOverflow = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_ULT, res, src0, "", &I);

            // llvm.uadd.with.overflow returns a struct, where the first element is the add result,
            // and the second is the overflow flag.
            // Replace each extract with the correct instruction.
            for(auto U = I.user_begin(), EU = I.user_end(); U != EU; ++U)
            {
                ExtractValueInst* extract = dyn_cast<ExtractValueInst>(*U);
                if (!extract)
                {
                    assert(0 && "Did not expect anything but an extract after uadd_with_overflow");
                    continue;
                }
                
                ArrayRef<unsigned int> indices = extract->getIndices();
                if (indices[0] == 0)
                {
                    extract->replaceAllUsesWith(res);
                }
                else if (indices[0] == 1)
                {
                    extract->replaceAllUsesWith(isOverflow);
                }
                else
                {
                    assert(0 && "Unexpected index when handling uadd_with_overflow");
                }
                
                m_instructionsToRemove.insert(extract);
            }

            m_instructionsToRemove.insert(&I);
            break;
        }
        case Intrinsic::assume:
            m_instructionsToRemove.insert(&I);
            break;
        case Intrinsic::sadd_with_overflow:
        case Intrinsic::usub_with_overflow:
        case Intrinsic::ssub_with_overflow:
        case Intrinsic::umul_with_overflow:
        case Intrinsic::smul_with_overflow:
            TODO("Handle the other with_overflow intrinsics");
            assert(0 && "Unhandled llvm.x.with.overflow intrinsic");
            break;
        default:
            break;
    }
    if (!m_ctx->platform.supportFP16Rounding() && I.getType()->isHalfTy()) {
        // On platform lacking of FP16 rounding, promote them to FP32 and
        // demote back.
        Intrinsic::ID IID = I.getIntrinsicID();
        switch (IID) {
        default:
            break;
        case llvm::Intrinsic::floor:
        case llvm::Intrinsic::ceil:
        case llvm::Intrinsic::trunc: {
            IRBuilder<> IRB(&I);
            Value *Val = IRB.CreateFPExt(I.getOperand(0), IRB.getFloatTy());
            Value *Callee = Intrinsic::getDeclaration(I.getParent()->getParent()->getParent(), IID, IRB.getFloatTy());
            Val = IRB.CreateCall(Callee, Val);
            Val = IRB.CreateFPTrunc(Val, I.getType());
            I.replaceAllUsesWith(Val);
            I.eraseFromParent();
            break;
        }
        }
    }
}

void Legalization::visitBasicBlock(llvm::BasicBlock &BB) {
    fpMap.clear();
}

void Legalization::PromoteFp16ToFp32OnGenSampleCall(llvm::CallInst &I)
{
    Value* args[16];
    const int args_size = I.getCalledFunction()->getFunctionType()->getNumParams();
    llvm::ArrayRef<Value*> arrayRef_params(args, args_size);
    GenIntrinsicInst *CI = llvm::dyn_cast<GenIntrinsicInst>(&I);

    llvm::SmallVector<Type*,4> types;
    llvm::Value* texture = nullptr;
    llvm::Value* sampler = nullptr;
    if (SampleIntrinsic* inst = llvm::dyn_cast<SampleIntrinsic>(&I))
    {
        texture = inst->getTextureValue();
        sampler = inst->getSamplerValue();
    }
    else if (SamplerGatherIntrinsic* inst = llvm::dyn_cast<SamplerGatherIntrinsic>(&I))
    {
        texture = inst->getTextureValue();
        sampler = inst->getSamplerValue();
    }
    if (texture && texture->getType()->isPointerTy())
    {
        types.resize(4);
        types[2] = texture->getType();
        types[3] = sampler->getType();
    }
    else
    {
        types.resize(2);
    }
    types[0] = I.getType();
    types[1] = Type::getFloatTy(I.getContext());

    for (int index = 0; index < args_size; index++)
    {
        Value* input = I.getOperand(index);
        if (input->getType()->isHalfTy())
        {
            m_builder->SetInsertPoint(&I);            
            if (fpMap.find(input) == fpMap.end())
            {
                args[index] = m_builder->CreateFPExt(input, Type::getFloatTy(I.getContext()), "");
                fpMap[input] = args[index];
            }
            else
            {
                args[index] = fpMap[input];
            }
        }
        else
        {
            args[index] = input;
        }
    }

    llvm::Function*  f0 = GenISAIntrinsic::getDeclaration(m_ctx->getModule(), CI->getIntrinsicID(), types);
    llvm::CallInst* I0 = GenIntrinsicInst::Create(f0, arrayRef_params, "", &I);
    I.replaceAllUsesWith(I0);
    I.eraseFromParent();
}

void Legalization::visitTruncInst(llvm::TruncInst &I) {
    // Legalize
    //
    //  (trunc (bitcast <3 x i16> to i48) i32)
    //
    // into
    //
    //  (or (extract-element <3 x i16> 0)
    //      (shl (extract-element <3 x i16> 1) 16))
    //
    // Or, legalize
    //
    //  (trunc (lshr (bitcast <3 x i16> to i48) 32)
    //
    // into
    //
    //  (or (extract-element <3 x i16> 2) 0)
    //

    Type *DstTy = I.getDestTy();
    if (!DstTy->isIntegerTy(32))
        return;
    if (!I.getSrcTy()->isIntegerTy(48))
        return;

    unsigned Idx = 0; // By default, extract from the 0th element.

    Value *Src = I.getOperand(0);
    BitCastInst *BC = dyn_cast<BitCastInst>(Src);
    if (!BC) {
        // Check (lshr ...)
        BinaryOperator *BO = dyn_cast<BinaryOperator>(Src);
        if (!BO)
            return;
        if (BO->getOpcode() != Instruction::LShr)
            return;
        // The shift amount must be constant.
        ConstantInt *CI = dyn_cast<ConstantInt>(BO->getOperand(1));
        if (!CI)
            return;
        if (CI->equalsInt(16))
            Idx = 1;
        else if (CI->equalsInt(32))
            Idx = 2;
        else // Bail out if the shift amount is not a mutiplication of 16.
            return;

        BC = dyn_cast<BitCastInst>(BO->getOperand(0));
        if (!BC)
            return;
    }

    Src = BC->getOperand(0);
    VectorType *VTy = dyn_cast<VectorType>(Src->getType());
    // Bail out if it's not bitcasted from <3 x i16>
    if (!VTy || VTy->getNumElements() != 3 || !VTy->getElementType()->isIntegerTy(16))
        return;

    m_builder->SetInsertPoint(&I);

    assert(Idx < 3 && "The initial index is out of range!");

    Value *NewVal =
        m_builder->CreateZExt(
            m_builder->CreateExtractElement(Src, m_builder->getInt32(Idx)), DstTy);
    if (++Idx < 3) {
        Value *Hi
            = m_builder->CreateZExt(
                m_builder->CreateExtractElement(Src, m_builder->getInt32(Idx)),
                DstTy);
        NewVal = m_builder->CreateOr(m_builder->CreateShl(Hi, 16), NewVal);
    }

    I.replaceAllUsesWith(NewVal);
    I.eraseFromParent();
}

void Legalization::visitAddrSpaceCastInst(llvm::AddrSpaceCastInst &I) {
    if (m_ctx->type != ShaderType::OPENCL_SHADER)
        return;

    Value *Src = I.getOperand(0);
    PointerType *SrcPtrTy = cast<PointerType>(Src->getType());
    if (SrcPtrTy->getAddressSpace() != ADDRESS_SPACE_LOCAL)
        return;

    PointerType *DstPtrTy = cast<PointerType>(I.getType());
    unsigned AS = DstPtrTy->getAddressSpace();
    if (AS != ADDRESS_SPACE_GENERIC) {
        if (!AS) // FIXME: Skip nullify on default AS as it's still used in VA builtins.
            return;
        Value *Null = Constant::getNullValue(DstPtrTy);
        I.replaceAllUsesWith(Null);
        I.eraseFromParent();
        return;
    }

    //Check for null pointer casting
    //Currently check is only handling specific scnario 
    //%n = addrspacecast i32 addrspace(3)* null to i32 addrspace(4)*
    //this will be replaced with "null", and the %n where is used will be replaced with null
    //This issue was exposed with LLVM 4.0 because of a patch
    //github.com/llvm-mirror/llvm/commit/bca8aba44a2f414a25b55a3ba37f718113315f5f#diff-11765a284352f0be6fc81f5d6a8ddcbc
    //This patch made sure, that above instructions are not replaced with null
    //Consequently Legalization pass had to appropriately handle it.
    //However current fix will not handle complex scenariod such as
    //local pointer casted to different address spaces in dynamic flow
    if(isa<ConstantPointerNull>(I.getPointerOperand())) {
        Constant *Null = Constant::getNullValue(I.getType());
        I.replaceAllUsesWith(Null);
        I.eraseFromParent();
        return;
    }

    Function *F = I.getParent()->getParent();
    ImplicitArgs implicitArgs = ImplicitArgs(*F, getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils());
    Argument *SLM = implicitArgs.getImplicitArg(*F, ImplicitArg::LOCAL_MEMORY_STATELESS_WINDOW_START_ADDRESS);
    if (!SLM)
        return;

    m_builder->SetInsertPoint(&I);

    unsigned PtrSz = m_DL->getPointerSizeInBits(cast<PointerType>(SLM->getType())->getAddressSpace());
    Type *Int16Ty = m_builder->getInt16Ty();
    Type *IntPtrTy = m_builder->getIntNTy(PtrSz);
    Value *Offset = m_builder->CreateZExt(m_builder->CreatePtrToInt(Src, Int16Ty), IntPtrTy);
    Value *Start = m_builder->CreatePtrToInt(SLM, IntPtrTy);
    Value *GASPtr = m_builder->CreateIntToPtr(m_builder->CreateAdd(Start, Offset), DstPtrTy);
    I.replaceAllUsesWith(GASPtr);
    I.eraseFromParent();
}

namespace {

/// Match and legalize IR that IGC does not handle correctly or efficiently; run
/// after some llvm optimization pass.
class GenOptLegalizer : public FunctionPass, public InstVisitor<GenOptLegalizer>
{
public:
    static char ID;
    GenOptLegalizer();
    bool runOnFunction(Function &F) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override
    {
        AU.setPreservesCFG();
    }

    void visitBitCastInst(BitCastInst &I);
    void visitLoadInst(LoadInst &I);
    void visitStoreInst(StoreInst &I);

private:
    const DataLayout *m_DL;
    IRBuilder<> *m_Builder;
    bool m_Changed;
    std::vector<llvm::Instruction*> m_InstructionsToRemove;
};

} // namespace

namespace IGC {

FunctionPass *createGenOptLegalizer()
{
    return new GenOptLegalizer();
}

} // namespace IGC

IGC_INITIALIZE_PASS_BEGIN(GenOptLegalizer, "GenOptLegalizer", "GenOptLegalizer", false, false)
IGC_INITIALIZE_PASS_END(GenOptLegalizer, "GenOptLegalizer", "GenOptLegalizer", false, false)

char GenOptLegalizer::ID = 0;
GenOptLegalizer::GenOptLegalizer()
    : FunctionPass(ID), m_DL(nullptr), m_Builder(nullptr), m_Changed(false)
{
    initializeGenOptLegalizerPass(*PassRegistry::getPassRegistry());
}

bool GenOptLegalizer::runOnFunction(Function &F)
{
    IRBuilder<> Builder(F.getContext());
    m_Builder = &Builder;
    m_DL = &F.getParent()->getDataLayout();
    m_Changed = false;
    m_InstructionsToRemove.clear();
    visit(F);
    for (auto I : m_InstructionsToRemove)
        I->eraseFromParent();
    m_InstructionsToRemove.clear();
    return m_Changed;
}

void GenOptLegalizer::visitBitCastInst(BitCastInst &I)
{
    m_Changed |= LegalizeGVNBitCastPattern(m_Builder, m_DL, I, nullptr);
}

void GenOptLegalizer::visitLoadInst(LoadInst &I) {
    if (I.getType()->isIntegerTy(24)) {
        if (!I.hasOneUse())
            return;
        auto *ZEI = dyn_cast<ZExtInst>(*I.user_begin());
        if (!ZEI || !ZEI->getType()->isIntegerTy(32))
            return;
        // Transforms the following sequence
        //
        // %0 = load i24, i24* %ptr
        // %1 = zext i24 %0 to i32
        //
        // into
        //
        // %newptr = bitcast i24* %ptr to <3 x i8>*
        // %0 = load <3 x i8>, <3 x i8>* %newptr
        // %1 = shufflevector <3 x i8> %0, <3 x i8> zeroinitializer, <i32 0, i32 1, i32 2, i32 3>
        // %2 = bitcast <4 x i8> %1 to i32
        // (RAUW)
        //
        m_Builder->SetInsertPoint(&I);
        Type *I8x3Ty = VectorType::get(m_Builder->getInt8Ty(), 3);
        Type *I8x3PtrTy = PointerType::get(I8x3Ty, I.getPointerAddressSpace());
        Value *NewPtr = m_Builder->CreateBitCast(I.getPointerOperand(), I8x3PtrTy);
        Value *NewLD = IGC::cloneLoad(&I, NewPtr);
        Type *NewTy = ZEI->getType();
        Value *NewVal = Constant::getNullValue(NewTy);
        Value *L0 = m_Builder->CreateExtractElement(NewLD, uint64_t(0));
        NewVal = m_Builder->CreateOr(NewVal,
                                     m_Builder->CreateShl(
                                         m_Builder->CreateZExt(L0, NewTy),
                                         uint64_t(0)));
        Value *L1 = m_Builder->CreateExtractElement(NewLD, uint64_t(1));
        NewVal = m_Builder->CreateOr(NewVal,
                                     m_Builder->CreateShl(
                                         m_Builder->CreateZExt(L1, NewTy),
                                         uint64_t(8)));
        Value *L2 = m_Builder->CreateExtractElement(NewLD, uint64_t(2));
        NewVal = m_Builder->CreateOr(NewVal,
                                     m_Builder->CreateShl(
                                         m_Builder->CreateZExt(L2, NewTy),
                                         uint64_t(16)));
        ZEI->replaceAllUsesWith(NewVal);
        m_InstructionsToRemove.push_back(ZEI);
        m_InstructionsToRemove.push_back(&I);
        m_Changed = true;
    }
}

void GenOptLegalizer::visitStoreInst(StoreInst &I) {
    Value *V = I.getValueOperand();
    if (V->getType()->isIntegerTy(24)) {
        if (!V->hasOneUse())
            return;
        if(LoadInst* LD = dyn_cast<LoadInst>(V))
        {
            // Transforms the following sequence
            //
            // %0 = load i24, i24* %src
            // %1 = store i24 %0, i24* %dst
            //
            // into
            //
            // %newsrc = bitcast i24* %src to <3 x i8>*
            // %0 = load <3 x i8>, <3 x i8>* %newsrc
            // %newdst = bitcast i24* %dst to <3 x i8>*
            // %1 = store <3 x i8> %0, <3 x i8>* %newdst
            //
            Type *I8x3Ty = VectorType::get(m_Builder->getInt8Ty(), 3);
            Type *I8x3PtrTy = PointerType::get(I8x3Ty, LD->getPointerAddressSpace());
            // Replace load of i24 to load of <3 x i8>
            m_Builder->SetInsertPoint(LD);
            Value *NewPtr = m_Builder->CreateBitCast(LD->getPointerOperand(), I8x3PtrTy);
            Value *NewLD = IGC::cloneLoad(LD, NewPtr);
            // Replace store of i24 to load of <3 x i8>
            m_Builder->SetInsertPoint(&I);
            I8x3PtrTy = PointerType::get(I8x3Ty, I.getPointerAddressSpace());
            NewPtr = m_Builder->CreateBitCast(I.getPointerOperand(), I8x3PtrTy);
            IGC::cloneStore(&I, NewLD, NewPtr);
            // Remove original LD and ST.
            m_InstructionsToRemove.push_back(&I);
            m_InstructionsToRemove.push_back(LD);
            m_Changed = true;
        }
        else
        {
            TruncInst* SV = dyn_cast<TruncInst>(I.getValueOperand());
            BitCastInst* SP = dyn_cast<BitCastInst>(I.getPointerOperand());
            if (SV && SP)
            {
                // Transforms the following sequence
                //
                // %0 = bitcast i8* %ptr to i24*
                // %1 = trunc i32 %src to i24
                // store i24 %1, i24 addrspace(1)* %0
                //
                // into
                //
                // %0 = bitcast i8* %ptr to <3 x i8>*
                // %1 = bitcast i32 %src to <4 x i8>
                // %2 = shufflevector <4 x i8> %1, <4 x i8> undef, <i32 0, i32 1, i32 2>
                // store <3 x i8> %2, <3 x i8>* %0
                //
                m_Builder->SetInsertPoint(&I);
                Type *I8x3Ty = VectorType::get(m_Builder->getInt8Ty(), 3);
                Type *I8x3PtrTy = PointerType::get(I8x3Ty, I.getPointerAddressSpace());

                // Convert i32 to <4 x i8>
                Type *SrcTy = SV->getOperand(0)->getType();
                unsigned numElements = SrcTy->getPrimitiveSizeInBits() / 8;
                Type *NewVecTy = VectorType::get(m_Builder->getInt8Ty(), numElements);
                Value *NewVec = m_Builder->CreateBitCast(SV->getOperand(0), NewVecTy);
                // Create shufflevector to select elements for <3 x i8>
                SmallVector<uint32_t, 3> maskVals = {0, 1, 2};
                Value *pMask = ConstantDataVector::get(I.getContext(), maskVals);
                auto *NewVal = new ShuffleVectorInst(NewVec, UndefValue::get(NewVecTy), pMask);
                NewVal->insertBefore(&I);
                // Bitcast src pointer to <3 x i8>* instead of i24*
                Value *NewPtr = m_Builder->CreateBitCast(SP->getOperand(0), I8x3PtrTy);
                // Create new store
                IGC::cloneStore(&I, NewVal, NewPtr);

                m_InstructionsToRemove.push_back(&I);
                m_InstructionsToRemove.push_back(SV);
                m_InstructionsToRemove.push_back(SP);
                m_Changed = true;
            }
        }
    }
}

static bool isCandidateFDiv(Instruction* Inst)
{
    if (Inst->use_empty())
        return false;

    Type* Ty = Inst->getType();
    if (!Ty->isFloatTy() && !Ty->isHalfTy())
        return false;

    auto Op = dyn_cast<FPMathOperator>(Inst);
    if (Op && Op->getOpcode() == Instruction::FDiv) {
        Value* Src0 = Op->getOperand(0);
        if (auto CFP = dyn_cast<ConstantFP>(Src0))
            return !CFP->isExactlyValue(1.0);
        return true;
    }
    return false;
}

// Check if a scaling factor is needed for a constant denominator.
static bool needsNoScaling(Value *Val)
{
    auto FP = dyn_cast<ConstantFP>(Val);
    if (!FP || !FP->getType()->isFloatTy())
        return false;

    union {
        uint32_t u32;
        float f32;
    } U;

    float FVal = FP->getValueAPF().convertToFloat();
    U.f32 = FVal;

    uint32_t UVal = U.u32;
    UVal &= 0x7f800000;
    return (UVal > 0) && (UVal < (200U << 23));
}

// Expand fdiv(x, y) into rcp(y * S) * x * S
// where S = 2^32 if exp(y) == 0,
//       S = 2^(-32) if exp(y) >= 200,
//       S = 1.0f otherwise
//
bool IGC::expandFDIVInstructions(llvm::Function& F)
{
    bool Changed = false;
    for (auto& BB : F.getBasicBlockList()) {
        for (auto Iter = BB.begin(); Iter != BB.end();) {
            Instruction* Inst = &*Iter++;
            if (!isCandidateFDiv(Inst))
                continue;

            IRBuilder<> Builder(Inst);
            Builder.setFastMathFlags(Inst->getFastMathFlags());

            auto& Ctx = Inst->getContext();
            Value* X = Inst->getOperand(0);
            Value* Y = Inst->getOperand(1);
            Value* V = nullptr;

            if (Inst->getType()->isHalfTy()) {
                if (Inst->hasAllowReciprocal()) {
                    APFloat Val(1.0f);
                    bool ignored;
                    Val.convert(APFloat::IEEEhalf(), APFloat::rmTowardZero, &ignored);
                    ConstantFP* C1 = ConstantFP::get(Ctx, Val);
                    Y = Builder.CreateFDiv(C1, Y);
                    V = Builder.CreateFMul(Y, X);
                } else {
                    // Up cast to float, do rcp+mul in float, and down cast to half.
                    Y = Builder.CreateFPExt(Y, Builder.getFloatTy());
                    Y = Builder.CreateFDiv(ConstantFP::get(Ctx, APFloat(1.0f)), Y);
                    X = Builder.CreateFPExt(X, Builder.getFloatTy());
                    V = Builder.CreateFMul(Y, X);
                    V = Builder.CreateFPTrunc(V, Inst->getType());
                }
            } else if (Inst->hasAllowReciprocal() || needsNoScaling(Y)) {
                Y = Builder.CreateFDiv(ConstantFP::get(Ctx, APFloat(1.0f)), Y);
                V = Builder.CreateFMul(Y, X);
            } else {
                float S32 = uint64_t(1) << 32;
                ConstantFP* C0 = ConstantFP::get(Ctx, APFloat(S32));
                ConstantFP* C1 = ConstantFP::get(Ctx, APFloat(1.0f));
                ConstantFP* C2 = ConstantFP::get(Ctx, APFloat(1.0f / S32));

                Value* Exp = Builder.CreateAnd(
                    Builder.CreateBitCast(Y, Builder.getInt32Ty()),
                    Builder.getInt32(0x7f800000));

                // Check if B's exponent is 0, scale up.
                Value* P1 = Builder.CreateICmpEQ(Exp, Builder.getInt32(0));
                Value* Scale = Builder.CreateSelect(P1, C0, C1);

                // Check if B's exponent >= 200, scale down.
                Value* P2 = Builder.CreateICmpUGE(Exp, Builder.getInt32(200 << 23));
                Scale = Builder.CreateSelect(P2, C2, Scale);

                // Compute rcp(y * S) * x * S
                V = Builder.CreateFMul(Y, Scale);
                V = Builder.CreateFDiv(C1, V);
                V = Builder.CreateFMul(V, X);
                V = Builder.CreateFMul(V, Scale);
            }

            Inst->replaceAllUsesWith(V);
            Inst->eraseFromParent();
            Changed = true;
        }
    }

    return Changed;
}

namespace IGC {

class GenFDIVEmulation : public FunctionPass {
public:
    static char ID;
    GenFDIVEmulation();
    bool runOnFunction(Function& F) override;
    void getAnalysisUsage(AnalysisUsage& AU) const override
    {
        AU.setPreservesCFG();
    }
};

FunctionPass* createGenFDIVEmulation()
{
    return new GenFDIVEmulation;
}

} // namespace IGC

IGC_INITIALIZE_PASS_BEGIN(GenFDIVEmulation, "GenFDIVEmulation", "GenFDIVEmulation", false, false)
IGC_INITIALIZE_PASS_END(GenFDIVEmulation, "GenFDIVEmulation", "GenFDIVEmulation", false, false)

char GenFDIVEmulation::ID = 0;
GenFDIVEmulation::GenFDIVEmulation()
    : FunctionPass(ID)
{
    initializeGenFDIVEmulationPass(*PassRegistry::getPassRegistry());
}

bool GenFDIVEmulation::runOnFunction(Function &F)
{
    // Always emulate fdiv instructions.
    return expandFDIVInstructions(F);
}
