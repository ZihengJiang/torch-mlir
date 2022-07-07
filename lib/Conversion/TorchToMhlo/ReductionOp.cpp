//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchToMhlo/TorchToMhlo.h"

#include "../PassDetail.h"
#include "./MhloLegalizeUtils.h"
#include "./PopulatePattern.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "torch-mlir/Conversion/Utils/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchDialect.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/TorchUpstream.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include "torch-mlir/Dialect/TorchConversion/IR/TorchConversionOps.h"

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

static Value createInitialValueForReduceOp(Operation *op, Type elementTy,
                                           PatternRewriter &rewriter) {
  auto constType = RankedTensorType::get({}, elementTy);
  if (isa<AtenSumOp, AtenSumDimIntListOp>(op)) {
    if (elementTy.isa<mlir::FloatType>()) {
      auto constAttr = DenseElementsAttr::get(
          constType, {APFloat::getZero(
                         elementTy.cast<mlir::FloatType>().getFloatSemantics(),
                         /*negative=*/false)});
      return rewriter.create<mhlo::ConstantOp>(op->getLoc(), constType,
                                               constAttr);
    } else if (elementTy.isa<mlir::IntegerType>() &&
               elementTy.getIntOrFloatBitWidth() != 8) {
      auto constAttr = DenseElementsAttr::get(
          constType, {APInt::getZero(elementTy.getIntOrFloatBitWidth())});
      return rewriter.create<mhlo::ConstantOp>(op->getLoc(), constType,
                                               constAttr);
    }
  }

  if (isa<AtenMaxOp, AtenMaxDimOp, AtenArgmaxOp>(op)) {
    if (elementTy.isa<mlir::FloatType>()) {
      auto constAttr = DenseElementsAttr::get(
          constType, {APFloat::getLargest(
                         elementTy.cast<mlir::FloatType>().getFloatSemantics(),
                         /*negative=*/true)});
      return rewriter.create<mhlo::ConstantOp>(op->getLoc(), constType,
                                               constAttr);
    } else if (elementTy.isa<mlir::IntegerType>() &&
               elementTy.getIntOrFloatBitWidth() != 8) {
      auto constAttr = DenseElementsAttr::get(
          constType,
          {APInt::getSignedMinValue(elementTy.getIntOrFloatBitWidth())});
      return rewriter.create<mhlo::ConstantOp>(op->getLoc(), constType,
                                               constAttr);
    }
  }

  op->emitError("unimplemented lowering in "
                "createInitialValueForReduceOp");
  return nullptr;
}

// Util for converting AtenArgmaxOp and AtenMaxDimOp
static llvm::Optional<ValueRange>
getMaxInDim(ConversionPatternRewriter &rewriter, Operation *op, Value &input,
            int64_t dim) {
  auto inputTy = input.getType().template cast<RankedTensorType>();
  if (!inputTy) {
    return llvm::None;
  }
  if (!inputTy.getElementType().isIntOrFloat()) {
    return llvm::None;
  }
  auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();

  Value init_val = createInitialValueForReduceOp(op, inputElemTy, rewriter);

  Value init_idx =
      mhlo::getConstTensor<int64_t>(rewriter, op, {0}, {}).getValue();

  DenseIntElementsAttr dimensions = DenseIntElementsAttr::get(
      RankedTensorType::get(1, rewriter.getI64Type()), dim);

  auto inputShapeConst =
      mhlo::getConstTensor(rewriter, op, inputShape,
                           {static_cast<int64_t>(inputShape.size())})
          .getValue();
  auto indexTensor = rewriter.create<mhlo::DynamicIotaOp>(
      op->getLoc(), RankedTensorType::get(inputShape, rewriter.getI64Type()),
      inputShapeConst, static_cast<uint64_t>(dim));

  auto mhloReduceOp = rewriter.create<mhlo::ReduceOp>(
      op->getLoc(), mlir::ValueRange{input, indexTensor},
      mlir::ValueRange{
          init_val,
          init_idx,
      },
      dimensions);

  Block &block = mhloReduceOp.body().emplaceBlock();

  // Add block arguments
  auto blockValArgumentType =
      RankedTensorType::get({}, inputTy.getElementType());
  auto blockIdxArgumentType = RankedTensorType::get({}, rewriter.getI64Type());
  auto compareResultType = RankedTensorType::get({}, rewriter.getI1Type());
  block.addArgument(blockValArgumentType, op->getLoc());
  block.addArgument(blockIdxArgumentType, op->getLoc());

  block.addArgument(blockValArgumentType, op->getLoc());
  block.addArgument(blockIdxArgumentType, op->getLoc());

  auto *firstValArg = block.args_begin();
  auto *firstIdxArg = std::next(firstValArg);
  auto *secondValArg = std::next(firstIdxArg);
  auto *secondIdxArg = std::next(secondValArg);

  mhlo::ComparisonTypeAttr compareTypeAttr;
  if (inputTy.getElementType().isa<mlir::FloatType>()) {
    compareTypeAttr = mhlo::ComparisonTypeAttr::get(
        rewriter.getContext(), mhlo::ComparisonType::FLOAT);
  } else if (inputTy.getElementType().isa<mlir::IntegerType>()) {
    compareTypeAttr = mhlo::ComparisonTypeAttr::get(
        rewriter.getContext(), mhlo::ComparisonType::SIGNED);
  }
  mhlo::ComparisonDirectionAttr compareGeDirectionAttr =
      mhlo::ComparisonDirectionAttr::get(rewriter.getContext(),
                                         mhlo::ComparisonDirection::GE);
  mhlo::ComparisonDirectionAttr compareEqDirectionAttr =
      mhlo::ComparisonDirectionAttr::get(rewriter.getContext(),
                                         mhlo::ComparisonDirection::EQ);

  {
    mlir::IRRewriter::InsertPoint prevIP = rewriter.saveInsertionPoint();

    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(&block);

    Value compareGeResult = rewriter.create<mhlo::CompareOp>(
        op->getLoc(), compareResultType, *firstValArg, *secondValArg,
        compareGeDirectionAttr, compareTypeAttr);
    Value retValResult = rewriter.create<mhlo::SelectOp>(
        op->getLoc(), compareGeResult, *firstValArg, *secondValArg);

    // get smaller index value if compared nums are equal.
    Value compareEqResult = rewriter.create<mhlo::CompareOp>(
        op->getLoc(), compareResultType, *firstValArg, *secondValArg,
        compareEqDirectionAttr, compareTypeAttr);
    Value minIdx =
        rewriter.create<mhlo::MinOp>(op->getLoc(), *firstIdxArg, *secondIdxArg);
    Value idxWithGeVal = rewriter.create<mhlo::SelectOp>(
        op->getLoc(), compareGeResult, *firstIdxArg, *secondIdxArg);
    Value retIdxResult = rewriter.create<mhlo::SelectOp>(
        op->getLoc(), compareEqResult, minIdx, idxWithGeVal);

    rewriter.create<mhlo::ReturnOp>(
        op->getLoc(), mlir::ValueRange{retValResult, retIdxResult});

    rewriter.restoreInsertionPoint(prevIP);
  }
  return mhloReduceOp.getResults();
}

namespace {
template <typename AtenOpT>
class ConvertAtenReductionOp : public OpConversionPattern<AtenOpT> {
public:
  using OpConversionPattern<AtenOpT>::OpConversionPattern;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override;
};
// AtenArgmaxOp
template <>
LogicalResult ConvertAtenReductionOp<AtenArgmaxOp>::matchAndRewrite(
    AtenArgmaxOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value input = adaptor.self();
  auto inputTy = input.getType().template cast<RankedTensorType>();
  if (!inputTy) {
    op.emitError("Only Tensor types supported in MHLO");
  }

  auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();
  if (!inputElemTy.isIntOrFloat()) {
    return op.emitError(
        "Only floating-point or integer datatype legalization supported");
  }
  // check if input element type is (u)int8
  if (inputElemTy.isa<mlir::IntegerType>() &&
      inputElemTy.getIntOrFloatBitWidth() == 8) {
    return rewriter.notifyMatchFailure(
        op, "IntegerType with bitwidth 8 unsupported in convertion from "
            "AtenArgmaxOp to MHLO");
  }

  int64_t dim;
  if (!matchPattern(op.dim(), m_TorchConstantInt(&dim))) {
    return rewriter.notifyMatchFailure(op, "non-int dim unsupported");
  }
  dim = toPositiveDim(dim, inputTy.getRank());
  if (!isValidDim(dim, inputTy.getRank())) {
    return rewriter.notifyMatchFailure(op, "dim is not a valid dim");
  }

  bool keepDim = false;
  if (!matchPattern(op.keepdim(), m_TorchConstantBool(&keepDim))) {
    return rewriter.notifyMatchFailure(op, "non-bool keepdim unsupported");
  }

  auto mhloReduceResults = getMaxInDim(rewriter, op, input, dim).getValue();

  if (keepDim) {
    SmallVector<int64_t> outShape;
    for (auto &d : inputShape) {
      outShape.push_back(d);
    }
    outShape[dim] = 1;
    llvm::Optional<Value> outShapeConst =
        mhlo::getConstTensor(rewriter, op, llvm::makeArrayRef(outShape),
                             {static_cast<int64_t>(outShape.size())});
    auto mhloReduceIndexResult = rewriter.create<mhlo::DynamicReshapeOp>(
        op->getLoc(), RankedTensorType::get(outShape, rewriter.getI64Type()),
        mhloReduceResults[1], outShapeConst.getValue());
    rewriter.replaceOp(op, mhloReduceIndexResult.getResult());
    return success();
  }

  rewriter.replaceOp(op, mhloReduceResults[1]);
  return success();
}

// AtenMaxDimOp
template <>
LogicalResult ConvertAtenReductionOp<AtenMaxDimOp>::matchAndRewrite(
    AtenMaxDimOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value input = adaptor.self();
  auto inputTy = input.getType().template dyn_cast<RankedTensorType>();
  if (!inputTy) {
    return op.emitError("Only Tensor types supported in MHLO");
  }
  auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();
  if (!inputElemTy.isIntOrFloat()) {
    return op.emitError(
        "Only floating-point or integer datatype legalization supported");
  }
  // check if input element type is (u)int8
  if (inputElemTy.isa<mlir::IntegerType>() &&
      inputElemTy.getIntOrFloatBitWidth() == 8) {
    return rewriter.notifyMatchFailure(
        op, "IntegerType with bitwidth 8 unsupported in convertion from "
            "AtenMaxDimOp to MHLO");
  }

  RankedTensorType valResultType = getTypeConverter()
                                       ->convertType(op.getResult(0).getType())
                                       .template cast<RankedTensorType>();
  RankedTensorType idxResultType = getTypeConverter()
                                       ->convertType(op.getResult(1).getType())
                                       .template cast<RankedTensorType>();
  Type idxElementType = idxResultType.getElementType();
  if (!idxElementType.isa<mlir::IntegerType>()) {
    return op.emitError("Aten.max.dim op needs integer-like result");
  }

  int64_t dim;
  if (!matchPattern(op.dim(), m_TorchConstantInt(&dim))) {
    return rewriter.notifyMatchFailure(op, "non-int dim unsupported");
  }
  dim = toPositiveDim(dim, inputTy.getRank());
  if (!isValidDim(dim, inputTy.getRank())) {
    return rewriter.notifyMatchFailure(op, "dim is not a valid dim");
  }
  bool keepDim = false;
  if (!matchPattern(op.keepdim(), m_TorchConstantBool(&keepDim))) {
    return rewriter.notifyMatchFailure(op, "non-bool keepdim unsupported");
  }

  auto mhloReduceResults = getMaxInDim(rewriter, op, input, dim).getValue();

  if (keepDim) {
    SmallVector<int64_t> outShape;
    for (auto &d : inputShape) {
      outShape.push_back(d);
    }
    outShape[dim] = 1;
    llvm::Optional<Value> outShapeConst =
        mhlo::getConstTensor(rewriter, op, llvm::makeArrayRef(outShape),
                             {static_cast<int64_t>(outShape.size())});
    auto mhloReduceValueResult = rewriter.create<mhlo::DynamicReshapeOp>(
        op->getLoc(), valResultType, mhloReduceResults[0],
        outShapeConst.getValue());
    auto mhloReduceIndexResult = rewriter.create<mhlo::DynamicReshapeOp>(
        op->getLoc(), idxResultType, mhloReduceResults[1],
        outShapeConst.getValue());
    rewriter.replaceOp(op, {mhloReduceValueResult, mhloReduceIndexResult});
    return success();
  }

  rewriter.replaceOp(op, {mhloReduceResults[0], mhloReduceResults[1]});
  return success();
}

// AtenSumOp
template <>
LogicalResult ConvertAtenReductionOp<AtenSumOp>::matchAndRewrite(
    AtenSumOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value input = adaptor.self();
  auto inputTy = input.getType().dyn_cast<RankedTensorType>();
  if (!inputTy) {
    return op.emitError("Only Tensor types supported in MHLO");
  }
  auto dtype = adaptor.dtype();
  if (!dtype.getType().isa<Torch::NoneType>()) {
    auto dstElemTy = getTypeConverter()
                         ->convertType(op.getType())
                         .template dyn_cast<RankedTensorType>()
                         .getElementType();
    input = rewriter.create<mhlo::ConvertOp>(op->getLoc(), input, dstElemTy);
    inputTy = input.getType().dyn_cast<RankedTensorType>();
  }
  // auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();
  if (!inputElemTy.isIntOrFloat()) {
    return op.emitError(
        "Only floating-point or integer datatype legalization supported");
  }
  // check if input element type is (u)int8
  if (inputElemTy.isa<mlir::IntegerType>() &&
      inputElemTy.getIntOrFloatBitWidth() == 8) {
    return rewriter.notifyMatchFailure(
        op, "IntegerType with bitwidth 8 unsupported in convertion from "
            "AtenSumOp to MHLO");
  }

  SmallVector<int64_t> dims;
  for (int64_t i = 0; i < inputTy.getRank(); i++) {
    dims.push_back(i);
  }

  Value init_value =
      createInitialValueForReduceOp(op, inputTy.getElementType(), rewriter);

  auto mhloReduceOp = rewriter.create<mhlo::ReduceOp>(
      op.getLoc(), input, init_value, rewriter.getI64TensorAttr(dims));

  Block &block = mhloReduceOp.body().emplaceBlock();
  auto blockArgumentTy = RankedTensorType::get({}, inputTy.getElementType());

  block.addArgument(blockArgumentTy, op->getLoc());
  block.addArgument(blockArgumentTy, op->getLoc());

  auto *firstArgument = block.args_begin();
  auto secondArgument = block.args_rbegin();

  {
    mlir::IRRewriter::InsertPoint prevIP = rewriter.saveInsertionPoint();
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(&block);
    Value addResult = rewriter.create<mhlo::AddOp>(
        op->getLoc(), blockArgumentTy, *firstArgument, *secondArgument);
    rewriter.create<mhlo::ReturnOp>(op->getLoc(), addResult);
    rewriter.restoreInsertionPoint(prevIP);
  }

  rewriter.replaceOp(op, mhloReduceOp.getResults());
  return success();
}

// AtenMaxOp
template <>
LogicalResult ConvertAtenReductionOp<AtenMaxOp>::matchAndRewrite(
    AtenMaxOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value input = adaptor.self();
  auto inputTy = input.getType().dyn_cast<RankedTensorType>();
  if (!inputTy) {
    return op.emitError("Only Tensor types supported in MHLO");
  }
  // auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();
  if (!inputElemTy.isIntOrFloat()) {
    return op.emitError(
        "Only floating-point or integer datatype legalization supported");
  }
  // Check if input element type is float
  // This is necessary, otherwise an error like
  // ""normal_kernel_cpu" not implemented for 'Long'(Int or Short)" will be
  // reported
  if (!inputElemTy.isa<mlir::FloatType>()) {
    return rewriter.notifyMatchFailure(op, "AtenMaxOp to MHLO "
                                           "requires Float input element type");
  }
  // check if input element type is (u)int8
  if (inputElemTy.isa<mlir::IntegerType>() &&
      inputElemTy.getIntOrFloatBitWidth() == 8) {
    return rewriter.notifyMatchFailure(
        op, "IntegerType with bitwidth 8 unsupported in convertion from "
            "AtenMaxOp to MHLO");
  }

  SmallVector<int64_t> dims;
  for (int64_t i = 0; i < inputTy.getRank(); i++) {
    dims.push_back(i);
  }

  Value init_value =
      createInitialValueForReduceOp(op, inputTy.getElementType(), rewriter);
  auto mhloReduceOp = rewriter.create<mhlo::ReduceOp>(
      op.getLoc(), input, init_value, rewriter.getI64TensorAttr(dims));

  Block &block = mhloReduceOp.body().emplaceBlock();
  auto blockArgumentTy = RankedTensorType::get({}, inputTy.getElementType());

  block.addArgument(blockArgumentTy, op->getLoc());
  block.addArgument(blockArgumentTy, op->getLoc());

  auto *firstArgument = block.args_begin();
  auto secondArgument = block.args_rbegin();

  {
    mlir::IRRewriter::InsertPoint prevIP = rewriter.saveInsertionPoint();
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(&block);
    Value maxResult = rewriter.create<mhlo::MaxOp>(
        op->getLoc(), blockArgumentTy, *firstArgument, *secondArgument);
    rewriter.create<mhlo::ReturnOp>(op->getLoc(), maxResult);
    rewriter.restoreInsertionPoint(prevIP);
  }

  rewriter.replaceOp(op, mhloReduceOp.getResults());
  return success();
}

// AtenSumDimIntListOp
template <>
LogicalResult ConvertAtenReductionOp<AtenSumDimIntListOp>::matchAndRewrite(
    AtenSumDimIntListOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Value input = adaptor.self();
  auto inputTy = input.getType().dyn_cast<RankedTensorType>();
  if (!inputTy) {
    return op.emitError("Only Tensor types supported in MHLO");
  }
  auto dtype = adaptor.dtype();
  if (!dtype.getType().isa<Torch::NoneType>()) {
    auto dstElemTy = getTypeConverter()
                         ->convertType(op.getType())
                         .template dyn_cast<RankedTensorType>()
                         .getElementType();
    input = rewriter.create<mhlo::ConvertOp>(op->getLoc(), input, dstElemTy);
    inputTy = input.getType().dyn_cast<RankedTensorType>();
  }
  auto inputShape = inputTy.getShape();
  auto inputElemTy = inputTy.getElementType();
  if (!inputElemTy.isIntOrFloat()) {
    return op.emitError(
        "Only floating-point or integer datatype legalization supported");
  }

  // check if input element type is (u)int8
  if (inputElemTy.isa<mlir::IntegerType>() &&
      inputElemTy.getIntOrFloatBitWidth() == 8) {
    return rewriter.notifyMatchFailure(
        op, "IntegerType with bitwidth 8 unsupported in convertion from "
            "AtenSumDimIntListOp to MHLO");
  }

  SmallVector<int64_t> inputDims;
  SmallVector<int64_t> dims;
  if (!matchPattern(op.dim(), m_TorchConstantIntList(inputDims))) {
    return rewriter.notifyMatchFailure(op, "non-int dim list unsupported");
  }

  for (auto d : inputDims) {
    d = toPositiveDim(d, inputTy.getRank());
    // drop invaid dim
    if (isValidDim(d, inputTy.getRank())) {
      dims.push_back(d);
    }
  }

  bool keepDim = false;
  if (!matchPattern(op.keepdim(), m_TorchConstantBool(&keepDim))) {
    return rewriter.notifyMatchFailure(op, "non-bool keepdim unsupported");
  }
  Value init_value =
      createInitialValueForReduceOp(op, inputTy.getElementType(), rewriter);

  auto mhloReduceOp = rewriter.create<mhlo::ReduceOp>(
      op.getLoc(), input, init_value, rewriter.getI64TensorAttr(dims));

  Region &region = mhloReduceOp.body();
  Block &block = region.emplaceBlock();
  auto blockArgumentTy = RankedTensorType::get({}, inputTy.getElementType());

  block.addArgument(blockArgumentTy, op->getLoc());
  block.addArgument(blockArgumentTy, op->getLoc());

  auto *firstArgument = block.args_begin();
  auto secondArgument = block.args_rbegin();

  {
    mlir::IRRewriter::InsertPoint prevIP = rewriter.saveInsertionPoint();
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(&block);
    Value addResult = rewriter.create<mhlo::AddOp>(
        op->getLoc(), blockArgumentTy, *firstArgument, *secondArgument);
    rewriter.create<mhlo::ReturnOp>(op->getLoc(), addResult);
    rewriter.restoreInsertionPoint(prevIP);
  }

  if (keepDim) {
    SmallVector<int64_t> outShape;
    for (auto &d : inputShape) {
      outShape.push_back(d);
    }
    for (int64_t &i : dims) {
      outShape[i] = 1;
    }
    llvm::Optional<Value> outShapeConst =
        mhlo::getConstTensor(rewriter, op, llvm::makeArrayRef(outShape),
                             {static_cast<int64_t>(outShape.size())});
    rewriter.replaceOpWithNewOp<mhlo::DynamicReshapeOp>(
        op, getTypeConverter()->convertType(op.getType()),
        mhloReduceOp.getResult(0), outShapeConst.getValue());
    return success();
  }
  rewriter.replaceOp(op, mhloReduceOp.getResults());
  return success();
}

} // namespace

void mlir::torch::torch_to_mhlo::populateReductionOpPatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target) {
  MLIRContext *context = patterns.getContext();
#define INSERT_ATEN_REDUCTION_OP_PATTERN(AtenOp)                               \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenReductionOp<AtenOp>>(typeConverter, context);
  INSERT_ATEN_REDUCTION_OP_PATTERN(AtenArgmaxOp);
  INSERT_ATEN_REDUCTION_OP_PATTERN(AtenMaxDimOp);
  INSERT_ATEN_REDUCTION_OP_PATTERN(AtenSumDimIntListOp);
  INSERT_ATEN_REDUCTION_OP_PATTERN(AtenSumOp);
  INSERT_ATEN_REDUCTION_OP_PATTERN(AtenMaxOp);
#undef INSERT_ATEN_REDUCTION_OP_PATTERN
}