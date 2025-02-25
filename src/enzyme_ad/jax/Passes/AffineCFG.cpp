#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "src/enzyme_ad/jax/Passes/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"
#include <deque>

#define DEBUG_TYPE "affine-cfg"

namespace mlir {
namespace enzyme {
#define GEN_PASS_DEF_AFFINECFG
#include "src/enzyme_ad/jax/Passes/Passes.h.inc"
} // namespace enzyme
} // namespace mlir

using namespace mlir;
using namespace mlir::arith;
using namespace mlir::affine;

bool isValidSymbolInt(Value value, bool recur = true);
bool isValidSymbolInt(Operation *defOp, bool recur) {
  Attribute operandCst;
  if (matchPattern(defOp, m_Constant(&operandCst)))
    return true;

  if (recur) {
    if (isa<SelectOp, IndexCastOp, IndexCastUIOp, AddIOp, MulIOp, DivSIOp,
            DivUIOp, RemSIOp, RemUIOp, SubIOp, CmpIOp, TruncIOp, ExtUIOp,
            ExtSIOp>(defOp))
      if (llvm::all_of(defOp->getOperands(), [&](Value v) {
            bool b = isValidSymbolInt(v, recur);
            // if (!b)
            //	LLVM_DEBUG(llvm::dbgs() << "illegal isValidSymbolInt: "
            //<< value << " due to " << v << "\n");
            return b;
          }))
        return true;
    if (auto ifOp = dyn_cast<scf::IfOp>(defOp)) {
      if (isValidSymbolInt(ifOp.getCondition(), recur)) {
        if (llvm::all_of(
                ifOp.thenBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }) &&
            llvm::all_of(
                ifOp.elseBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }))
          return true;
      }
    }
    if (auto ifOp = dyn_cast<affine::AffineIfOp>(defOp)) {
      if (llvm::all_of(ifOp.getOperands(),
                       [&](Value o) { return isValidSymbolInt(o, recur); }))
        if (llvm::all_of(
                ifOp.getThenBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }) &&
            llvm::all_of(
                ifOp.getElseBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }))
          return true;
    }
  }
  return false;
}

// isValidSymbol, even if not index
bool isValidSymbolInt(Value value, bool recur) {
  // Check that the value is a top level value.
  if (affine::isTopLevelValue(value))
    return true;

  if (auto *defOp = value.getDefiningOp()) {
    if (isValidSymbolInt(defOp, recur))
      return true;
    return affine::isValidSymbol(value, affine::getAffineScope(defOp));
  }

  return false;
}

struct AffineApplyNormalizer {
  AffineApplyNormalizer(AffineMap map, ArrayRef<Value> operands,
                        PatternRewriter &rewriter, DominanceInfo &DI);

  /// Returns the AffineMap resulting from normalization.
  AffineMap getAffineMap() { return affineMap; }

  SmallVector<Value, 8> getOperands() {
    SmallVector<Value, 8> res(reorderedDims);
    res.append(concatenatedSymbols.begin(), concatenatedSymbols.end());
    return res;
  }

private:
  /// Helper function to insert `v` into the coordinate system of the current
  /// AffineApplyNormalizer. Returns the AffineDimExpr with the corresponding
  /// renumbered position.
  AffineDimExpr renumberOneDim(Value v);

  /// Maps of Value to position in `affineMap`.
  DenseMap<Value, unsigned> dimValueToPosition;

  /// Ordered dims and symbols matching positional dims and symbols in
  /// `affineMap`.
  SmallVector<Value, 8> reorderedDims;
  SmallVector<Value, 8> concatenatedSymbols;

  AffineMap affineMap;
};

static bool isAffineForArg(Value val) {
  if (!val.isa<BlockArgument>())
    return false;
  Operation *parentOp = val.cast<BlockArgument>().getOwner()->getParentOp();
  return (
      isa_and_nonnull<affine::AffineForOp, affine::AffineParallelOp>(parentOp));
}

static bool legalCondition(Value en, bool dim = false) {
  if (en.getDefiningOp<affine::AffineApplyOp>())
    return true;

  if (!dim && !isValidSymbolInt(en, /*recur*/ false)) {
    if (isValidIndex(en) || isValidSymbolInt(en, /*recur*/ true)) {
      return true;
    }
  }

  while (auto ic = en.getDefiningOp<IndexCastOp>())
    en = ic.getIn();

  while (auto ic = en.getDefiningOp<IndexCastUIOp>())
    en = ic.getIn();

  if ((en.getDefiningOp<AddIOp>() || en.getDefiningOp<SubIOp>() ||
       en.getDefiningOp<MulIOp>() || en.getDefiningOp<RemUIOp>() ||
       en.getDefiningOp<RemSIOp>()) &&
      (en.getDefiningOp()->getOperand(1).getDefiningOp<ConstantIntOp>() ||
       en.getDefiningOp()->getOperand(1).getDefiningOp<ConstantIndexOp>()))
    return true;
  // if (auto IC = dyn_cast_or_null<IndexCastOp>(en.getDefiningOp())) {
  //	if (!outer || legalCondition(IC.getOperand(), false)) return true;
  //}
  if (!dim)
    if (auto BA = dyn_cast<BlockArgument>(en)) {
      if (isa<affine::AffineForOp, affine::AffineParallelOp>(
              BA.getOwner()->getParentOp()))
        return true;
    }
  return false;
}

/// The AffineNormalizer composes AffineApplyOp recursively. Its purpose is to
/// keep a correspondence between the mathematical `map` and the `operands` of
/// a given affine::AffineApplyOp. This correspondence is maintained by
/// iterating over the operands and forming an `auxiliaryMap` that can be
/// composed mathematically with `map`. To keep this correspondence in cases
/// where symbols are produced by affine.apply operations, we perform a local
/// rewrite of symbols as dims.
///
/// Rationale for locally rewriting symbols as dims:
/// ================================================
/// The mathematical composition of AffineMap must always concatenate symbols
/// because it does not have enough information to do otherwise. For example,
/// composing `(d0)[s0] -> (d0 + s0)` with itself must produce
/// `(d0)[s0, s1] -> (d0 + s0 + s1)`.
///
/// The result is only equivalent to `(d0)[s0] -> (d0 + 2 * s0)` when
/// applied to the same mlir::Value for both s0 and s1.
/// As a consequence mathematical composition of AffineMap always concatenates
/// symbols.
///
/// When AffineMaps are used in affine::AffineApplyOp however, they may specify
/// composition via symbols, which is ambiguous mathematically. This corner case
/// is handled by locally rewriting such symbols that come from
/// affine::AffineApplyOp into dims and composing through dims.
/// TODO: Composition via symbols comes at a significant code
/// complexity. Alternatively we should investigate whether we want to
/// explicitly disallow symbols coming from affine.apply and instead force the
/// user to compose symbols beforehand. The annoyances may be small (i.e. 1 or 2
/// extra API calls for such uses, which haven't popped up until now) and the
/// benefit potentially big: simpler and more maintainable code for a
/// non-trivial, recursive, procedure.
AffineApplyNormalizer::AffineApplyNormalizer(AffineMap map,
                                             ArrayRef<Value> operands,
                                             PatternRewriter &rewriter,
                                             DominanceInfo &DI) {
  assert(map.getNumInputs() == operands.size() &&
         "number of operands does not match the number of map inputs");

  LLVM_DEBUG(map.print(llvm::dbgs() << "\nInput map: "));

  SmallVector<Value, 8> addedValues;

  llvm::SmallSet<unsigned, 1> symbolsToPromote;

  unsigned numDims = map.getNumDims();
  unsigned numSymbols = map.getNumSymbols();

  SmallVector<AffineExpr, 8> dimReplacements;
  SmallVector<AffineExpr, 8> symReplacements;

  SmallVector<SmallVectorImpl<Value> *> opsTodos;
  auto replaceOp = [&](Operation *oldOp, Operation *newOp) {
    for (auto [oldV, newV] :
         llvm::zip(oldOp->getResults(), newOp->getResults()))
      for (auto ops : opsTodos)
        for (auto &op : *ops)
          if (op == oldV)
            op = newV;
  };

  std::function<Value(Value, bool)> fix = [&](Value v,
                                              bool index) -> Value /*legal*/ {
    if (isValidSymbolInt(v, /*recur*/ false))
      return v;
    if (index && isAffineForArg(v))
      return v;
    auto *op = v.getDefiningOp();
    if (!op)
      return nullptr;
    if (!op)
      llvm::errs() << v << "\n";
    assert(op);
    if (isa<ConstantOp>(op) || isa<ConstantIndexOp>(op))
      return v;
    if (!isReadOnly(op)) {
      return nullptr;
    }
    Operation *front = nullptr;
    SmallVector<Value> ops;
    opsTodos.push_back(&ops);
    std::function<void(Operation *)> getAllOps = [&](Operation *todo) {
      for (auto v : todo->getOperands()) {
        if (llvm::all_of(op->getRegions(), [&](Region &r) {
              return !r.isAncestor(v.getParentRegion());
            }))
          ops.push_back(v);
      }
      for (auto &r : todo->getRegions()) {
        for (auto &b : r.getBlocks())
          for (auto &o2 : b.without_terminator())
            getAllOps(&o2);
      }
    };
    getAllOps(op);
    for (auto o : ops) {
      Operation *next;
      if (auto *op = o.getDefiningOp()) {
        if (Value nv = fix(o, index)) {
          op = nv.getDefiningOp();
        } else {
          return nullptr;
        }
        next = op->getNextNode();
      } else {
        auto BA = o.cast<BlockArgument>();
        if (index && isAffineForArg(BA)) {
        } else if (!isValidSymbolInt(o, /*recur*/ false)) {
          return nullptr;
        }
        next = &BA.getOwner()->front();
      }
      if (front == nullptr)
        front = next;
      else if (DI.dominates(front, next))
        front = next;
    }
    opsTodos.pop_back();
    if (!front)
      op->dump();
    assert(front);
    PatternRewriter::InsertionGuard B(rewriter);
    rewriter.setInsertionPoint(front);
    auto cloned = rewriter.clone(*op);
    replaceOp(op, cloned);
    rewriter.replaceOp(op, cloned->getResults());
    return cloned->getResult(0);
  };
  auto renumberOneSymbol = [&](Value v) {
    for (auto i : llvm::enumerate(addedValues)) {
      if (i.value() == v)
        return getAffineSymbolExpr(i.index(), map.getContext());
    }
    auto expr = getAffineSymbolExpr(addedValues.size(), map.getContext());
    addedValues.push_back(v);
    return expr;
  };

  // 2. Compose affine::AffineApplyOps and dispatch dims or symbols.
  for (unsigned i = 0, e = operands.size(); i < e; ++i) {
    auto t = operands[i];
    auto decast = t;
    while (true) {
      if (auto idx = decast.getDefiningOp<IndexCastOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<IndexCastUIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<TruncIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<ExtUIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<ExtSIOp>()) {
        decast = idx.getIn();
        continue;
      }
      break;
    }

    if (!isValidSymbolInt(t, /*recur*/ false)) {
      t = decast;
    }

    // Only promote one at a time, lest we end up with two dimensions
    // multiplying each other.

    if (((!isValidSymbolInt(t, /*recur*/ false) &&
          (t.getDefiningOp<AddIOp>() || t.getDefiningOp<SubIOp>() ||
           (t.getDefiningOp<MulIOp>() &&
            ((isValidIndex(t.getDefiningOp()->getOperand(0)) &&
              isValidSymbolInt(t.getDefiningOp()->getOperand(1))) ||
             (isValidIndex(t.getDefiningOp()->getOperand(1)) &&
              isValidSymbolInt(t.getDefiningOp()->getOperand(0)))) &&
            !(fix(t.getDefiningOp()->getOperand(0), false) &&
              fix(t.getDefiningOp()->getOperand(1), false))

                ) ||
           ((t.getDefiningOp<DivUIOp>() || t.getDefiningOp<DivSIOp>()) &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1))) &&
            (!(fix(t.getDefiningOp()->getOperand(0), false) &&
               fix(t.getDefiningOp()->getOperand(1), false)))) ||
           (t.getDefiningOp<DivSIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<DivUIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<RemUIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<RemSIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           t.getDefiningOp<ConstantIntOp>() ||
           t.getDefiningOp<ConstantIndexOp>())) ||
         ((decast.getDefiningOp<AddIOp>() || decast.getDefiningOp<SubIOp>() ||
           decast.getDefiningOp<MulIOp>() || decast.getDefiningOp<RemUIOp>() ||
           decast.getDefiningOp<RemSIOp>() || decast.getDefiningOp<ShRUIOp>() ||
           decast.getDefiningOp<ShLIOp>()) &&
          (decast.getDefiningOp()
               ->getOperand(1)
               .getDefiningOp<ConstantIntOp>() ||
           decast.getDefiningOp()
               ->getOperand(1)
               .getDefiningOp<ConstantIndexOp>())))) {
      t = decast;
      LLVM_DEBUG(llvm::dbgs() << " Replacing: " << t << "\n");

      AffineMap affineApplyMap;
      SmallVector<Value, 8> affineApplyOperands;

      // llvm::dbgs() << "\nop to start: " << t << "\n";

      if (auto op = t.getDefiningOp<AddIOp>()) {
        affineApplyMap =
            AffineMap::get(0, 2,
                           getAffineSymbolExpr(0, op.getContext()) +
                               getAffineSymbolExpr(1, op.getContext()));
        affineApplyOperands.push_back(op.getLhs());
        affineApplyOperands.push_back(op.getRhs());
      } else if (auto op = t.getDefiningOp<SubIOp>()) {
        affineApplyMap =
            AffineMap::get(0, 2,
                           getAffineSymbolExpr(0, op.getContext()) -
                               getAffineSymbolExpr(1, op.getContext()));
        affineApplyOperands.push_back(op.getLhs());
        affineApplyOperands.push_back(op.getRhs());
      } else if (auto op = t.getDefiningOp<MulIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) * ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) * ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) *
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<DivSIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap = AffineMap::get(
              0, 2,
              getAffineSymbolExpr(0, op.getContext())
                  .floorDiv(getAffineSymbolExpr(1, op.getContext())));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<DivUIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap = AffineMap::get(
              0, 2,
              getAffineSymbolExpr(0, op.getContext())
                  .floorDiv(getAffineSymbolExpr(1, op.getContext())));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<RemSIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) %
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<RemUIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) %
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<ShRUIOp>()) {

        APInt iattr;
        if (!matchPattern(op.getRhs(), m_ConstantInt(&iattr))) {
          llvm_unreachable("shr rhs needed to be constant int");
        }

        affineApplyMap =
            AffineMap::get(0, 1,
                           getAffineSymbolExpr(0, op.getContext())
                               .floorDiv(1 << iattr.getZExtValue()));
        affineApplyOperands.push_back(op.getLhs());
      } else if (auto op = t.getDefiningOp<ShLIOp>()) {

        APInt iattr;
        if (!matchPattern(op.getRhs(), m_ConstantInt(&iattr))) {
          llvm_unreachable("shl rhs needed to be constant int");
        }

        affineApplyMap =
            AffineMap::get(0, 1,
                           getAffineSymbolExpr(0, op.getContext()) *
                               (1 << iattr.getZExtValue()));
        affineApplyOperands.push_back(op.getLhs());
      } else if (auto op = t.getDefiningOp<ConstantIntOp>()) {
        affineApplyMap = AffineMap::get(
            0, 0, getAffineConstantExpr(op.value(), op.getContext()));
      } else if (auto op = t.getDefiningOp<ConstantIndexOp>()) {
        affineApplyMap = AffineMap::get(
            0, 0, getAffineConstantExpr(op.value(), op.getContext()));
      } else {
        llvm_unreachable("");
      }

      SmallVector<AffineExpr, 0> dimRemapping;
      unsigned numOtherSymbols = affineApplyOperands.size();
      SmallVector<AffineExpr, 2> symRemapping(numOtherSymbols);
      for (unsigned idx = 0; idx < numOtherSymbols; ++idx) {
        symRemapping[idx] = renumberOneSymbol(affineApplyOperands[idx]);
      }
      affineApplyMap = affineApplyMap.replaceDimsAndSymbols(
          dimRemapping, symRemapping, reorderedDims.size(), addedValues.size());

      LLVM_DEBUG(affineApplyMap.print(
          llvm::dbgs() << "\nRenumber into current normalizer: "));

      if (i >= numDims)
        symReplacements.push_back(affineApplyMap.getResult(0));
      else
        dimReplacements.push_back(affineApplyMap.getResult(0));

    } else if (isAffineForArg(t)) {
      if (i >= numDims)
        symReplacements.push_back(renumberOneDim(t));
      else
        dimReplacements.push_back(renumberOneDim(t));
    } else if (t.getDefiningOp<affine::AffineApplyOp>()) {
      auto affineApply = t.getDefiningOp<affine::AffineApplyOp>();
      // a. Compose affine.apply operations.
      LLVM_DEBUG(affineApply->print(
          llvm::dbgs() << "\nCompose affine::AffineApplyOp recursively: "));
      AffineMap affineApplyMap = affineApply.getAffineMap();
      SmallVector<Value, 8> affineApplyOperands(
          affineApply.getOperands().begin(), affineApply.getOperands().end());

      SmallVector<AffineExpr, 0> dimRemapping(affineApplyMap.getNumDims());

      for (size_t i = 0; i < affineApplyMap.getNumDims(); ++i) {
        assert(i < affineApplyOperands.size());
        dimRemapping[i] = renumberOneDim(affineApplyOperands[i]);
      }
      unsigned numOtherSymbols = affineApplyOperands.size();
      SmallVector<AffineExpr, 2> symRemapping(numOtherSymbols -
                                              affineApplyMap.getNumDims());
      for (unsigned idx = 0; idx < symRemapping.size(); ++idx) {
        symRemapping[idx] = renumberOneSymbol(
            affineApplyOperands[idx + affineApplyMap.getNumDims()]);
      }
      affineApplyMap = affineApplyMap.replaceDimsAndSymbols(
          dimRemapping, symRemapping, reorderedDims.size(), addedValues.size());

      LLVM_DEBUG(
          affineApplyMap.print(llvm::dbgs() << "\nAffine apply fixup map: "));

      if (i >= numDims)
        symReplacements.push_back(affineApplyMap.getResult(0));
      else
        dimReplacements.push_back(affineApplyMap.getResult(0));
    } else {
      if (!isValidSymbolInt(t, /*recur*/ false)) {
        if (t.getDefiningOp()) {
          if ((t = fix(t, false))) {
            assert(isValidSymbolInt(t, /*recur*/ false));
          } else
            llvm_unreachable("cannot move");
        } else
          llvm_unreachable("cannot move2");
      }
      if (i < numDims) {
        // b. The mathematical composition of AffineMap composes dims.
        dimReplacements.push_back(renumberOneDim(t));
      } else {
        // c. The mathematical composition of AffineMap concatenates symbols.
        //    Note that the map composition will put symbols already present
        //    in the map before any symbols coming from the auxiliary map, so
        //    we insert them before any symbols that are due to renumbering,
        //    and after the proper symbols we have seen already.
        symReplacements.push_back(renumberOneSymbol(t));
      }
    }
  }
  for (auto v : addedValues)
    concatenatedSymbols.push_back(v);

  // Create the new map by replacing each symbol at pos by the next new dim.
  unsigned numNewDims = reorderedDims.size();
  unsigned numNewSymbols = addedValues.size();
  assert(dimReplacements.size() == map.getNumDims());
  assert(symReplacements.size() == map.getNumSymbols());
  auto auxillaryMap = map.replaceDimsAndSymbols(
      dimReplacements, symReplacements, numNewDims, numNewSymbols);
  LLVM_DEBUG(auxillaryMap.print(llvm::dbgs() << "\nRewritten map: "));

  affineMap = auxillaryMap; // simplifyAffineMap(auxillaryMap);

  LLVM_DEBUG(affineMap.print(llvm::dbgs() << "\nSimplified result: "));
  LLVM_DEBUG(llvm::dbgs() << "\n");
}

AffineDimExpr AffineApplyNormalizer::renumberOneDim(Value v) {
  DenseMap<Value, unsigned>::iterator iterPos;
  bool inserted = false;
  std::tie(iterPos, inserted) =
      dimValueToPosition.insert(std::make_pair(v, dimValueToPosition.size()));
  if (inserted) {
    reorderedDims.push_back(v);
  }
  return getAffineDimExpr(iterPos->second, v.getContext())
      .cast<AffineDimExpr>();
}

static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value> *operands,
                                        PatternRewriter &rewriter,
                                        DominanceInfo &DI) {
  AffineApplyNormalizer normalizer(*map, *operands, rewriter, DI);
  auto normalizedMap = normalizer.getAffineMap();
  auto normalizedOperands = normalizer.getOperands();
  affine::canonicalizeMapAndOperands(&normalizedMap, &normalizedOperands);
  *map = normalizedMap;
  *operands = normalizedOperands;
  assert(*map);
}

bool need(AffineMap *map, SmallVectorImpl<Value> *operands) {
  assert(map->getNumInputs() == operands->size());
  for (size_t i = 0; i < map->getNumInputs(); ++i) {
    auto v = (*operands)[i];
    if (legalCondition(v, i < map->getNumDims()))
      return true;
  }
  return false;
}
bool need(IntegerSet *map, SmallVectorImpl<Value> *operands) {
  for (size_t i = 0; i < map->getNumInputs(); ++i) {
    auto v = (*operands)[i];
    if (legalCondition(v, i < map->getNumDims()))
      return true;
  }
  return false;
}

void fully2ComposeAffineMapAndOperands(PatternRewriter &builder, AffineMap *map,
                                       SmallVectorImpl<Value> *operands,
                                       DominanceInfo &DI) {
  IRMapping indexMap;
  for (auto op : *operands) {
    SmallVector<IndexCastOp> attempt;
    auto idx0 = op.getDefiningOp<IndexCastOp>();
    attempt.push_back(idx0);
    if (!idx0)
      continue;

    for (auto &u : idx0.getIn().getUses()) {
      if (auto idx = dyn_cast<IndexCastOp>(u.getOwner()))
        if (DI.dominates((Operation *)idx, &*builder.getInsertionPoint()))
          attempt.push_back(idx);
    }

    for (auto idx : attempt) {
      if (affine::isValidSymbol(idx)) {
        indexMap.map(idx.getIn(), idx);
        break;
      }
    }
  }
  assert(map->getNumInputs() == operands->size());
  while (need(map, operands)) {
    composeAffineMapAndOperands(map, operands, builder, DI);
    assert(map->getNumInputs() == operands->size());
  }
  *map = simplifyAffineMap(*map);
  for (auto &op : *operands) {
    if (!op.getType().isIndex()) {
      Operation *toInsert;
      if (auto *o = op.getDefiningOp())
        toInsert = o->getNextNode();
      else {
        auto BA = op.cast<BlockArgument>();
        toInsert = &BA.getOwner()->front();
      }

      if (auto v = indexMap.lookupOrNull(op))
        op = v;
      else {
        PatternRewriter::InsertionGuard B(builder);
        builder.setInsertionPoint(toInsert);
        op = builder.create<IndexCastOp>(op.getLoc(), builder.getIndexType(),
                                         op);
      }
    }
  }
}

void fully2ComposeIntegerSetAndOperands(PatternRewriter &builder,
                                        IntegerSet *set,
                                        SmallVectorImpl<Value> *operands,
                                        DominanceInfo &DI) {
  IRMapping indexMap;
  for (auto op : *operands) {
    SmallVector<IndexCastOp> attempt;
    auto idx0 = op.getDefiningOp<IndexCastOp>();
    attempt.push_back(idx0);
    if (!idx0)
      continue;

    for (auto &u : idx0.getIn().getUses()) {
      if (auto idx = dyn_cast<IndexCastOp>(u.getOwner()))
        if (DI.dominates((Operation *)idx, &*builder.getInsertionPoint()))
          attempt.push_back(idx);
    }

    for (auto idx : attempt) {
      if (affine::isValidSymbol(idx)) {
        indexMap.map(idx.getIn(), idx);
        break;
      }
    }
  }
  auto map = AffineMap::get(set->getNumDims(), set->getNumSymbols(),
                            set->getConstraints(), set->getContext());
  while (need(&map, operands)) {
    composeAffineMapAndOperands(&map, operands, builder, DI);
  }
  map = simplifyAffineMap(map);
  *set = IntegerSet::get(map.getNumDims(), map.getNumSymbols(),
                         map.getResults(), set->getEqFlags());
  for (auto &op : *operands) {
    if (!op.getType().isIndex()) {
      Operation *toInsert;
      if (auto *o = op.getDefiningOp())
        toInsert = o->getNextNode();
      else {
        auto BA = op.cast<BlockArgument>();
        toInsert = &BA.getOwner()->front();
      }

      if (auto v = indexMap.lookupOrNull(op))
        op = v;
      else {
        PatternRewriter::InsertionGuard B(builder);
        builder.setInsertionPoint(toInsert);
        op = builder.create<IndexCastOp>(op.getLoc(), builder.getIndexType(),
                                         op);
      }
    }
  }
}

namespace {
struct AffineCFGPass : public enzyme::impl::AffineCFGBase<AffineCFGPass> {
  void runOnOperation() override;
};
} // namespace

static void setLocationAfter(PatternRewriter &b, mlir::Value val) {
  if (val.getDefiningOp()) {
    auto it = val.getDefiningOp()->getIterator();
    it++;
    b.setInsertionPoint(val.getDefiningOp()->getBlock(), it);
  }
  if (auto bop = dyn_cast<mlir::BlockArgument>(val))
    b.setInsertionPoint(bop.getOwner(), bop.getOwner()->begin());
}

template <typename T> struct IndexCastMovement : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    if (op.use_empty()) {
      rewriter.eraseOp(op);
      return success();
    }

    mlir::Value val = op.getOperand();
    if (auto bop = dyn_cast<mlir::BlockArgument>(val)) {
      if (op.getOperation()->getBlock() != bop.getOwner()) {
        op.getOperation()->moveBefore(bop.getOwner(), bop.getOwner()->begin());
        return success();
      }
      return failure();
    }

    if (val.getDefiningOp()) {
      if (op.getOperation()->getBlock() != val.getDefiningOp()->getBlock()) {
        auto it = val.getDefiningOp()->getIterator();
        op.getOperation()->moveAfter(val.getDefiningOp()->getBlock(), it);
      }
      return failure();
    }
    return failure();
  }
};

/*
struct SimplfyIntegerCastMath : public OpRewritePattern<IndexCastOp> {
  using OpRewritePattern<IndexCastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IndexCastOp op,
                                PatternRewriter &rewriter) const override {
    if (op.use_empty()) {
      rewriter.eraseOp(op);
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<AddIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<AddIOp>(
          op,
          b.create<IndexCastOp>(op.getLoc(), op.getType(), iadd.getOperand(0)),
          b2.create<IndexCastOp>(op.getLoc(), op.getType(),
                                 iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<SubIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<SubIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<MulIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<MulIOp>(
          op,
          b.create<IndexCastOp>(op.getLoc(), op.getType(), iadd.getOperand(0)),
          b2.create<IndexCastOp>(op.getLoc(), op.getType(),
                                 iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<DivUIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<DivUIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<DivSIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<DivSIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<RemUIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<RemUIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<RemSIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<RemSIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<SelectOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getTrueValue());
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getFalseValue());
      auto cond = iadd.getCondition();
      PatternRewriter b3(rewriter);
      setLocationAfter(b3, cond);
      if (auto cmp = iadd.getCondition().getDefiningOp<CmpIOp>()) {
        if (cmp.getLhs() == iadd.getTrueValue() &&
            cmp.getRhs() == iadd.getFalseValue()) {

          auto truev = b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                                    iadd.getTrueValue());
          auto falsev = b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                                      iadd.getFalseValue());
          cond = b3.create<CmpIOp>(cmp.getLoc(), cmp.getPredicate(), truev,
                                   falsev);
          rewriter.replaceOpWithNewOp<SelectOp>(op, cond, truev, falsev);
          return success();
        }
      }
    }
    return failure();
  }
};
*/

struct CanonicalizeAffineApply
    : public OpRewritePattern<affine::AffineApplyOp> {
  using OpRewritePattern<affine::AffineApplyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineApplyOp affineOp,
                                PatternRewriter &rewriter) const override {

    SmallVector<Value, 4> mapOperands(affineOp.getMapOperands());
    auto map = affineOp.getMap();
    auto prevMap = map;

    auto *scope = affine::getAffineScope(affineOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &map, &mapOperands, DI);
    affine::canonicalizeMapAndOperands(&map, &mapOperands);
    map = removeDuplicateExprs(map);

    if (map == prevMap)
      return failure();

    rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(affineOp, map,
                                                       mapOperands);
    return success();
  }
};

template <typename T>
struct CanonicalizeIndexCast : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T indexcastOp,
                                PatternRewriter &rewriter) const override {

    // Fold IndexCast(IndexCast(x)) -> x
    auto cast = indexcastOp.getOperand().template getDefiningOp<T>();
    if (cast && cast.getOperand().getType() == indexcastOp.getType()) {
      mlir::Value vals[] = {cast.getOperand()};
      rewriter.replaceOp(indexcastOp, vals);
      return success();
    }

    // Fold IndexCast(constant) -> constant
    // A little hack because we go through int.  Otherwise, the size
    // of the constant might need to change.
    if (auto cst =
            indexcastOp.getOperand().template getDefiningOp<ConstantIntOp>()) {
      rewriter.replaceOpWithNewOp<ConstantIndexOp>(indexcastOp, cst.value());
      return success();
    }
    return failure();
  }
};

/*
struct CanonicalizeAffineIf : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(affine::AffineIfOp affineOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> mapOperands(affineOp.mapOperands());
    auto map = affineOp.map();
    auto prevMap = map;
    fully2ComposeAffineMapAndOperands(&map, &mapOperands);
    affine::canonicalizeMapAndOperands(&map, &mapOperands);
    map = removeDuplicateExprs(map);
    if (map == prevMap)
      return failure();
    rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(affineOp, map,
mapOperands); return success();
  }
};
*/

bool isValidIndex(Value val) {
  if (isValidSymbolInt(val))
    return true;

  if (auto cast = val.getDefiningOp<IndexCastOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<IndexCastUIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<TruncIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<ExtSIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<ExtUIOp>())
    return isValidIndex(cast.getOperand());

  if (auto bop = val.getDefiningOp<AddIOp>())
    return isValidIndex(bop.getOperand(0)) && isValidIndex(bop.getOperand(1));

  if (auto bop = val.getDefiningOp<MulIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1))) ||
           (isValidIndex(bop.getOperand(1)) &&
            isValidSymbolInt(bop.getOperand(0)));

  if (auto bop = val.getDefiningOp<DivSIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1)));

  if (auto bop = val.getDefiningOp<DivUIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1)));

  if (auto bop = val.getDefiningOp<RemSIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (auto bop = val.getDefiningOp<RemUIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());

  if (auto bop = val.getDefiningOp<SubIOp>())
    return isValidIndex(bop.getOperand(0)) && isValidIndex(bop.getOperand(1));

  if (auto bop = val.getDefiningOp<ShRUIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (auto bop = val.getDefiningOp<ShLIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (val.getDefiningOp<ConstantIndexOp>())
    return true;

  if (val.getDefiningOp<ConstantIntOp>())
    return true;

  if (auto ba = dyn_cast<BlockArgument>(val)) {
    auto *owner = ba.getOwner();
    assert(owner);

    auto *parentOp = owner->getParentOp();
    if (!parentOp) {
      owner->dump();
      llvm::errs() << " ba: " << ba << "\n";
    }
    assert(parentOp);
    if (isa<FunctionOpInterface>(parentOp))
      return true;
    if (auto af = dyn_cast<affine::AffineForOp>(parentOp))
      return af.getInductionVar() == ba;

    // TODO ensure not a reduced var
    if (isa<affine::AffineParallelOp>(parentOp))
      return true;

    if (isa<FunctionOpInterface>(parentOp))
      return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "illegal isValidIndex: " << val << "\n");
  return false;
}

// returns legality
bool handleMinMax(Value start, SmallVectorImpl<Value> &out, bool &min,
                  bool &max) {

  SmallVector<Value> todo = {start};
  while (todo.size()) {
    auto cur = todo.back();
    todo.pop_back();
    if (isValidIndex(cur)) {
      out.push_back(cur);
      continue;
    } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
      // UB only has min of operands
      if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
        if (cmp.getLhs() == selOp.getTrueValue() &&
            cmp.getRhs() == selOp.getFalseValue()) {
          todo.push_back(cmp.getLhs());
          todo.push_back(cmp.getRhs());
          if (cmp.getPredicate() == CmpIPredicate::sle ||
              cmp.getPredicate() == CmpIPredicate::slt) {
            min = true;
            continue;
          }
          if (cmp.getPredicate() == CmpIPredicate::sge ||
              cmp.getPredicate() == CmpIPredicate::sgt) {
            max = true;
            continue;
          }
        }
      }
    }
    return false;
  }
  return !(min && max);
}

bool handle(PatternRewriter &b, CmpIOp cmpi, SmallVectorImpl<AffineExpr> &exprs,
            SmallVectorImpl<bool> &eqflags, SmallVectorImpl<Value> &applies,
            bool negated) {
  SmallVector<Value> lhs;
  bool lhs_min = false;
  bool lhs_max = false;
  if (!handleMinMax(cmpi.getLhs(), lhs, lhs_min, lhs_max)) {
    LLVM_DEBUG(llvm::dbgs()
               << "illegal lhs: " << cmpi.getLhs() << " - " << cmpi << "\n");
    return false;
  }
  assert(lhs.size());
  SmallVector<Value> rhs;
  bool rhs_min = false;
  bool rhs_max = false;
  if (!handleMinMax(cmpi.getRhs(), rhs, rhs_min, rhs_max)) {
    LLVM_DEBUG(llvm::dbgs()
               << "illegal rhs: " << cmpi.getRhs() << " - " << cmpi << "\n");
    return false;
  }
  assert(rhs.size());
  for (auto &lhspack : lhs)
    if (!lhspack.getType().isa<IndexType>()) {
      lhspack = b.create<arith::IndexCastOp>(
          cmpi.getLoc(), IndexType::get(cmpi.getContext()), lhspack);
    }

  for (auto &rhspack : rhs)
    if (!rhspack.getType().isa<IndexType>()) {
      rhspack = b.create<arith::IndexCastOp>(
          cmpi.getLoc(), IndexType::get(cmpi.getContext()), rhspack);
    }

  auto pred = cmpi.getPredicate();
  if (negated)
    pred = arith::invertPredicate(pred);

  switch (pred) {
  case CmpIPredicate::eq: {
    if (lhs_min || lhs_max || rhs_min || rhs_max)
      return false;
    eqflags.push_back(true);

    applies.push_back(lhs[0]);
    applies.push_back(rhs[0]);
    AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                          b.getAffineSymbolExpr(2 * exprs.size() + 1)};
    exprs.push_back(dims[0] - dims[1]);
  } break;

  case CmpIPredicate::ugt:
  case CmpIPredicate::uge:
    for (auto lhspack : lhs)
      if (!valueCmp(Cmp::GE, lhspack, 0)) {
        APInt ival;
        if (matchPattern(lhspack, m_ConstantInt(&ival))) {
          assert(ival.isNegative());
          assert(ival.isSingleWord());
          // Via Alive2: https://alive2.llvm.org/ce/z/5Fk78i
          //
          // if lhs >= 0, (as checked from above)
          // then this is correct with signed vs unsigned so long as the rhs !=
          // just the sign bit.
          if (ival.isMinSignedValue()) {
            LLVM_DEBUG(llvm::dbgs() << "illegal const greater lhs icmp: "
                                    << cmpi << " - " << lhspack << "\n");
            return false;
          }
        } else {
          LLVM_DEBUG(llvm::dbgs() << "illegal greater lhs icmp: " << cmpi
                                  << " - " << lhspack << "\n");
          return false;
        }
      }
    for (auto &rhspack : rhs)
      if (!valueCmp(Cmp::GE, rhspack, 0)) {
        APInt ival;
        if (matchPattern(rhspack, m_ConstantInt(&ival))) {
          assert(ival.isNegative());
          assert(ival.isSingleWord());
          // Via Alive2: https://alive2.llvm.org/ce/z/5Fk78i
          //
          // if lhs >= 0, (as checked from above)
          // then this is correct with signed vs unsigned so long as the rhs !=
          // just the sign bit.
          if (ival.isMinSignedValue()) {
            LLVM_DEBUG(llvm::dbgs() << "illegal const greater rhs icmp: "
                                    << cmpi << " - " << rhspack << "\n");
            return false;
          }
        } else {
          LLVM_DEBUG(llvm::dbgs() << "illegal greater rhs icmp: " << cmpi
                                  << " - " << rhspack << "\n");
          return false;
        }
      }

  case CmpIPredicate::sge:
  case CmpIPredicate::sgt: {
    // if lhs >=? rhs
    // if lhs is a min(a, b) both must be true and this is fine
    // if lhs is a max(a, b) either may be true, and sets require and
    // similarly if rhs is a max(), both must be true;
    if (lhs_max || rhs_min)
      return false;
    for (auto lhspack : lhs)
      for (auto rhspack : rhs) {
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(rhspack);
        AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                              b.getAffineSymbolExpr(2 * exprs.size() + 1)};
        auto expr = dims[0] - dims[1];
        if (cmpi.getPredicate() == CmpIPredicate::sgt ||
            cmpi.getPredicate() == CmpIPredicate::ugt)
          expr = expr - 1;
        exprs.push_back(expr);
      }
  } break;

  case CmpIPredicate::ult:
  case CmpIPredicate::ule:
    for (auto lhspack : lhs) {
      if (!valueCmp(Cmp::GE, lhspack, 0)) {
        // Assuming the rhs is strictly positive, even if the lhs is non
        // positive, we can add this as an additional check, that lhs >= 0.
        // Therefore lhs unsigned< rhs -> lhs signed< rhs && lhs >= 0
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(lhspack);
        AffineExpr expr = b.getAffineSymbolExpr(2 * exprs.size() + 0);
        exprs.push_back(expr);
      }
    }
    for (auto rhspack : rhs)
      if (!valueCmp(Cmp::GE, rhspack, 0)) {
        LLVM_DEBUG(llvm::dbgs() << "illegal less rhs icmp: " << cmpi << " - "
                                << rhspack << "\n");
        return false;
      }

  case CmpIPredicate::slt:
  case CmpIPredicate::sle: {
    if (lhs_min || rhs_max)
      return false;
    for (auto lhspack : lhs)
      for (auto rhspack : rhs) {
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(rhspack);
        AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                              b.getAffineSymbolExpr(2 * exprs.size() + 1)};
        auto expr = dims[1] - dims[0];
        if (cmpi.getPredicate() == CmpIPredicate::slt ||
            cmpi.getPredicate() == CmpIPredicate::ult)
          expr = expr - 1;
        exprs.push_back(expr);
      }
  } break;

  case CmpIPredicate::ne:
    LLVM_DEBUG(llvm::dbgs() << "illegal icmp: " << cmpi << "\n");
    return false;
  }
  return true;
}
/*
static void replaceStore(memref::StoreOp store,
                         const SmallVector<Value, 2> &newIndexes) {
  auto memrefType = store.getMemRef().getType().cast<MemRefType>();
  size_t rank = memrefType.getRank();
  if (rank != newIndexes.size()) {
    llvm::errs() << store << "\n";
  }
  assert(rank == newIndexes.size() && "Expect rank to match new indexes");

  PatternRewriter builder(store);
  Location loc = store.getLoc();
  builder.create<affine::AffineStoreOp>(loc, store.getValueToStore(),
store.getMemRef(), newIndexes); store.erase();
}

static void replaceLoad(memref::LoadOp load,
                        const SmallVector<Value, 2> &newIndexes) {
  PatternRewriter builder(load);
  Location loc = load.getLoc();

  auto memrefType = load.getMemRef().getType().cast<MemRefType>();
  size_t rank = memrefType.getRank();
  if (rank != newIndexes.size()) {
    llvm::errs() << load << "\n";
  }
  assert(rank == newIndexes.size() && "rank must equal new indexes size");

  affine::AffineLoadOp affineLoad =
      builder.create<affine::AffineLoadOp>(loc, load.getMemRef(), newIndexes);
  load.getResult().replaceAllUsesWith(affineLoad.getResult());
  load.erase();
}
*/
struct MoveLoadToAffine : public OpRewritePattern<memref::LoadOp> {
  using OpRewritePattern<memref::LoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::LoadOp load,
                                PatternRewriter &rewriter) const override {
    if (!llvm::all_of(load.getIndices(), isValidIndex))
      return failure();

    auto memrefType = load.getMemRef().getType().cast<MemRefType>();
    int64_t rank = memrefType.getRank();

    // Create identity map for memrefs with at least one dimension or () -> ()
    // for zero-dimensional memrefs.
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(rewriter.getAffineSymbolExpr(i));
    auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                              rewriter.getContext());

    SmallVector<Value, 4> operands = load.getIndices();

    if (map.getNumInputs() != operands.size()) {
      // load->getParentOfType<FuncOp>().dump();
      llvm::errs() << " load: " << load << "\n";
    }
    auto *scope = affine::getAffineScope(load)->getParentOp();
    DominanceInfo DI(scope);
    assert(map.getNumInputs() == operands.size());
    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    assert(map.getNumInputs() == operands.size());
    affine::canonicalizeMapAndOperands(&map, &operands);
    assert(map.getNumInputs() == operands.size());

    affine::AffineLoadOp affineLoad = rewriter.create<affine::AffineLoadOp>(
        load.getLoc(), load.getMemRef(), map, operands);
    load.getResult().replaceAllUsesWith(affineLoad.getResult());
    rewriter.eraseOp(load);
    return success();
  }
};

struct MoveStoreToAffine : public OpRewritePattern<memref::StoreOp> {
  using OpRewritePattern<memref::StoreOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::StoreOp store,
                                PatternRewriter &rewriter) const override {
    if (!llvm::all_of(store.getIndices(), isValidIndex))
      return failure();

    auto memrefType = store.getMemRef().getType().cast<MemRefType>();
    int64_t rank = memrefType.getRank();

    // Create identity map for memrefs with at least one dimension or () -> ()
    // for zero-dimensional memrefs.
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(rewriter.getAffineSymbolExpr(i));
    auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                              rewriter.getContext());
    SmallVector<Value, 4> operands = store.getIndices();

    auto *scope = affine::getAffineScope(store)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    affine::canonicalizeMapAndOperands(&map, &operands);

    rewriter.create<affine::AffineStoreOp>(store.getLoc(),
                                           store.getValueToStore(),
                                           store.getMemRef(), map, operands);
    rewriter.eraseOp(store);
    return success();
  }
};

static bool areChanged(SmallVectorImpl<Value> &afterOperands,
                       SmallVectorImpl<Value> &beforeOperands) {
  if (afterOperands.size() != beforeOperands.size())
    return true;
  if (!std::equal(afterOperands.begin(), afterOperands.end(),
                  beforeOperands.begin()))
    return true;
  return false;
}

template <typename T> struct AffineFixup : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  /// Replace the affine op with another instance of it with the supplied
  /// map and mapOperands.
  void replaceAffineOp(PatternRewriter &rewriter, T affineOp, AffineMap map,
                       ArrayRef<Value> mapOperands) const;

  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    auto map = op.getAffineMap();
    SmallVector<Value, 4> operands = op.getMapOperands();

    auto prevMap = map;
    auto prevOperands = operands;

    auto *scope = affine::getAffineScope(op)->getParentOp();
    DominanceInfo DI(scope);

    assert(map.getNumInputs() == operands.size());
    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    assert(map.getNumInputs() == operands.size());
    affine::canonicalizeMapAndOperands(&map, &operands);
    assert(map.getNumInputs() == operands.size());

    if (map == prevMap && !areChanged(operands, prevOperands))
      return failure();

    replaceAffineOp(rewriter, op, map, operands);
    return success();
  }
};

// Specialize the template to account for the different build signatures for
// affine load, store, and apply ops.
template <>
void AffineFixup<affine::AffineLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineLoadOp load, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineLoadOp>(load, load.getMemRef(), map,
                                                    mapOperands);
}
template <>
void AffineFixup<affine::AffinePrefetchOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffinePrefetchOp prefetch, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffinePrefetchOp>(
      prefetch, prefetch.getMemref(), map, mapOperands,
      prefetch.getLocalityHint(), prefetch.getIsWrite(),
      prefetch.getIsDataCache());
}
template <>
void AffineFixup<affine::AffineStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineStoreOp store, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineStoreOp>(
      store, store.getValueToStore(), store.getMemRef(), map, mapOperands);
}
template <>
void AffineFixup<affine::AffineVectorLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineVectorLoadOp vectorload,
    AffineMap map, ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineVectorLoadOp>(
      vectorload, vectorload.getVectorType(), vectorload.getMemRef(), map,
      mapOperands);
}
template <>
void AffineFixup<affine::AffineVectorStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineVectorStoreOp vectorstore,
    AffineMap map, ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineVectorStoreOp>(
      vectorstore, vectorstore.getValueToStore(), vectorstore.getMemRef(), map,
      mapOperands);
}

// Generic version for ops that don't have extra operands.
template <typename AffineOpTy>
void AffineFixup<AffineOpTy>::replaceAffineOp(
    PatternRewriter &rewriter, AffineOpTy op, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineOpTy>(op, map, mapOperands);
}

struct CanonicalieForBounds : public OpRewritePattern<affine::AffineForOp> {
  using OpRewritePattern<affine::AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
    SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());
    SmallVector<Value, 4> origLbOperands(forOp.getLowerBoundOperands());
    SmallVector<Value, 4> origUbOperands(forOp.getUpperBoundOperands());

    auto lbMap = forOp.getLowerBoundMap();
    auto ubMap = forOp.getUpperBoundMap();
    auto prevLbMap = lbMap;
    auto prevUbMap = ubMap;

    // llvm::errs() << "*********\n";
    // ubMap.dump();

    auto *scope = affine::getAffineScope(forOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbOperands, DI);
    affine::canonicalizeMapAndOperands(&lbMap, &lbOperands);
    lbMap = removeDuplicateExprs(lbMap);

    fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubOperands, DI);
    affine::canonicalizeMapAndOperands(&ubMap, &ubOperands);
    ubMap = removeDuplicateExprs(ubMap);

    // ubMap.dump();
    // forOp.dump();

    // Any canonicalization change in map or operands always leads to updated
    // map(s).
    if ((lbMap == prevLbMap && ubMap == prevUbMap) &&
        (!areChanged(lbOperands, origLbOperands)) &&
        (!areChanged(ubOperands, origUbOperands)))
      return failure();

    // llvm::errs() << "oldParent:" << *forOp.getParentOp() << "\n";
    // llvm::errs() << "oldfor:" << forOp << "\n";

    if ((lbMap != prevLbMap) || areChanged(lbOperands, origLbOperands))
      forOp.setLowerBound(lbOperands, lbMap);
    if ((ubMap != prevUbMap) || areChanged(ubOperands, origUbOperands))
      forOp.setUpperBound(ubOperands, ubMap);

    // llvm::errs() << "newfor:" << forOp << "\n";
    return success();
  }
};

struct CanonicalizIfBounds : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> operands(op.getOperands());
    SmallVector<Value, 4> origOperands(operands);

    auto map = op.getIntegerSet();
    auto prevMap = map;

    // llvm::errs() << "*********\n";
    // ubMap.dump();

    auto *scope = affine::getAffineScope(op)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeIntegerSetAndOperands(rewriter, &map, &operands, DI);
    affine::canonicalizeSetAndOperands(&map, &operands);

    // map(s).
    if (map == prevMap && !areChanged(operands, origOperands))
      return failure();

    op.setConditional(map, operands);

    return success();
  }
};

struct MoveIfToAffine : public OpRewritePattern<scf::IfOp> {
  using OpRewritePattern<scf::IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::IfOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (!ifOp->getParentOfType<affine::AffineForOp>() &&
        !ifOp->getParentOfType<affine::AffineParallelOp>())
      return failure();

    std::vector<mlir::Type> types;
    for (auto v : ifOp.getResults()) {
      types.push_back(v.getType());
    }

    SmallVector<AffineExpr, 2> exprs;
    SmallVector<bool, 2> eqflags;
    SmallVector<Value, 4> applies;

    // condition, Negated
    std::deque<std::pair<Value, bool>> todo = {
        std::make_pair(ifOp.getCondition(), false)};
    while (todo.size()) {
      auto &&[cur, negated] = todo.front();
      todo.pop_front();
      if (auto cmpi = cur.getDefiningOp<CmpIOp>()) {
        if (!handle(rewriter, cmpi, exprs, eqflags, applies, negated)) {
          return failure();
        }
        continue;
      }
      if (!negated) {
        if (auto andi = cur.getDefiningOp<AndIOp>()) {
          todo.emplace_back(andi.getOperand(0), negated);
          todo.emplace_back(andi.getOperand(1), negated);
          continue;
        }
      }
      if (negated) {
        if (auto andi = cur.getDefiningOp<OrIOp>()) {
          todo.emplace_back(andi.getOperand(0), negated);
          todo.emplace_back(andi.getOperand(1), negated);
          continue;
        }
      }

      if (auto noti = cur.getDefiningOp<XOrIOp>()) {
        if (matchPattern(noti.getOperand(1), m_One())) {
          todo.emplace_back(noti.getOperand(0), !negated);
          continue;
        }
      }
      LLVM_DEBUG(llvm::dbgs() << "illegal condition: " << cur
                              << " - negated: " << negated << "\n");
      return failure();
    }

    auto *scope = affine::getAffineScope(ifOp)->getParentOp();
    DominanceInfo DI(scope);

    auto iset =
        IntegerSet::get(/*dim*/ 0, /*symbol*/ 2 * exprs.size(), exprs, eqflags);
    fully2ComposeIntegerSetAndOperands(rewriter, &iset, &applies, DI);
    affine::canonicalizeSetAndOperands(&iset, &applies);
    affine::AffineIfOp affineIfOp =
        rewriter.create<affine::AffineIfOp>(ifOp.getLoc(), types, iset, applies,
                                            /*elseBlock=*/true);

    rewriter.setInsertionPoint(ifOp.thenYield());
    rewriter.replaceOpWithNewOp<affine::AffineYieldOp>(
        ifOp.thenYield(), ifOp.thenYield().getOperands());

    rewriter.eraseBlock(affineIfOp.getThenBlock());
    rewriter.eraseBlock(affineIfOp.getElseBlock());
    if (ifOp.getElseRegion().getBlocks().size()) {
      rewriter.setInsertionPoint(ifOp.elseYield());
      rewriter.replaceOpWithNewOp<affine::AffineYieldOp>(
          ifOp.elseYield(), ifOp.elseYield().getOperands());
    }

    rewriter.inlineRegionBefore(ifOp.getThenRegion(),
                                affineIfOp.getThenRegion(),
                                affineIfOp.getThenRegion().begin());
    rewriter.inlineRegionBefore(ifOp.getElseRegion(),
                                affineIfOp.getElseRegion(),
                                affineIfOp.getElseRegion().begin());

    rewriter.replaceOp(ifOp, affineIfOp.getResults());
    return success();
  }
};

struct ForOpRaising : public OpRewritePattern<scf::ForOp> {
  using OpRewritePattern<scf::ForOp>::OpRewritePattern;

  // TODO: remove me or rename me.
  bool isAffine(scf::ForOp loop) const {
    // return true;
    // enforce step to be a ConstantIndexOp (maybe too restrictive).
    APInt apint;
    return affine::isValidSymbol(loop.getStep()) ||
           matchPattern(loop.getStep(), m_ConstantInt(&apint));
  }

  int64_t getStep(mlir::Value value) const {
    APInt apint;
    if (matchPattern(value, m_ConstantInt(&apint)))
      return apint.getZExtValue();
    else
      return 1;
  }

  AffineMap getMultiSymbolIdentity(Builder &B, unsigned rank) const {
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(B.getAffineSymbolExpr(i));
    return AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                          B.getContext());
  }
  LogicalResult matchAndRewrite(scf::ForOp loop,
                                PatternRewriter &rewriter) const final {
    if (isAffine(loop)) {
      OpBuilder builder(loop);

      SmallVector<Value> lbs;
      {
        SmallVector<Value> todo = {loop.getLowerBound()};
        while (todo.size()) {
          auto cur = todo.back();
          todo.pop_back();
          if (isValidIndex(cur)) {
            lbs.push_back(cur);
            continue;
          } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
            // LB only has max of operands
            if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
              if (cmp.getLhs() == selOp.getTrueValue() &&
                  cmp.getRhs() == selOp.getFalseValue() &&
                  cmp.getPredicate() == CmpIPredicate::sge) {
                todo.push_back(cmp.getLhs());
                todo.push_back(cmp.getRhs());
                continue;
              }
            }
          }
          return failure();
        }
      }

      SmallVector<Value> ubs;
      {
        SmallVector<Value> todo = {loop.getUpperBound()};
        while (todo.size()) {
          auto cur = todo.back();
          todo.pop_back();
          if (isValidIndex(cur)) {
            ubs.push_back(cur);
            continue;
          } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
            // UB only has min of operands
            if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
              if (cmp.getLhs() == selOp.getTrueValue() &&
                  cmp.getRhs() == selOp.getFalseValue() &&
                  cmp.getPredicate() == CmpIPredicate::sle) {
                todo.push_back(cmp.getLhs());
                todo.push_back(cmp.getRhs());
                continue;
              }
            }
          }
          return failure();
        }
      }

      bool rewrittenStep = false;
      if (!loop.getStep().getDefiningOp<ConstantIndexOp>()) {
        if (ubs.size() != 1 || lbs.size() != 1)
          return failure();
        ubs[0] = rewriter.create<DivUIOp>(
            loop.getLoc(),
            rewriter.create<AddIOp>(
                loop.getLoc(),
                rewriter.create<SubIOp>(
                    loop.getLoc(), loop.getStep(),
                    rewriter.create<ConstantIndexOp>(loop.getLoc(), 1)),
                rewriter.create<SubIOp>(loop.getLoc(), loop.getUpperBound(),
                                        loop.getLowerBound())),
            loop.getStep());
        lbs[0] = rewriter.create<ConstantIndexOp>(loop.getLoc(), 0);
        rewrittenStep = true;
      }

      auto *scope = affine::getAffineScope(loop)->getParentOp();
      DominanceInfo DI(scope);

      AffineMap lbMap = getMultiSymbolIdentity(builder, lbs.size());
      {
        fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbs, DI);
        affine::canonicalizeMapAndOperands(&lbMap, &lbs);
        lbMap = removeDuplicateExprs(lbMap);
      }
      AffineMap ubMap = getMultiSymbolIdentity(builder, ubs.size());
      {
        fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubs, DI);
        affine::canonicalizeMapAndOperands(&ubMap, &ubs);
        ubMap = removeDuplicateExprs(ubMap);
      }

      affine::AffineForOp affineLoop = rewriter.create<affine::AffineForOp>(
          loop.getLoc(), lbs, lbMap, ubs, ubMap, getStep(loop.getStep()),
          loop.getInits());

      auto mergedYieldOp =
          cast<scf::YieldOp>(loop.getRegion().front().getTerminator());

      Block &newBlock = affineLoop.getRegion().front();

      // The terminator is added if the iterator args are not provided.
      // see the ::build method.
      if (affineLoop.getNumIterOperands() == 0) {
        auto *affineYieldOp = newBlock.getTerminator();
        rewriter.eraseOp(affineYieldOp);
      }

      SmallVector<Value> vals;
      rewriter.setInsertionPointToStart(&affineLoop.getRegion().front());
      for (Value arg : affineLoop.getRegion().front().getArguments()) {
        if (arg == affineLoop.getInductionVar() &&
            arg.getType() != loop.getInductionVar().getType()) {
          arg = rewriter.create<arith::IndexCastOp>(
              loop.getLoc(), loop.getInductionVar().getType(), arg);
        }
        if (rewrittenStep && arg == affineLoop.getInductionVar()) {
          arg = rewriter.create<AddIOp>(
              loop.getLoc(), loop.getLowerBound(),
              rewriter.create<MulIOp>(loop.getLoc(), arg, loop.getStep()));
        }
        vals.push_back(arg);
      }
      assert(vals.size() == loop.getRegion().front().getNumArguments());
      rewriter.mergeBlocks(&loop.getRegion().front(),
                           &affineLoop.getRegion().front(), vals);

      rewriter.setInsertionPoint(mergedYieldOp);
      rewriter.create<affine::AffineYieldOp>(mergedYieldOp.getLoc(),
                                             mergedYieldOp.getOperands());
      rewriter.eraseOp(mergedYieldOp);

      rewriter.replaceOp(loop, affineLoop.getResults());

      return success();
    }
    return failure();
  }
};

struct ParallelOpRaising : public OpRewritePattern<scf::ParallelOp> {
  using OpRewritePattern<scf::ParallelOp>::OpRewritePattern;

  void canonicalizeLoopBounds(PatternRewriter &rewriter,
                              affine::AffineParallelOp forOp) const {
    SmallVector<Value, 4> lbOperands(forOp.getLowerBoundsOperands());
    SmallVector<Value, 4> ubOperands(forOp.getUpperBoundsOperands());

    auto lbMap = forOp.getLowerBoundsMap();
    auto ubMap = forOp.getUpperBoundsMap();

    auto *scope = affine::getAffineScope(forOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbOperands, DI);
    affine::canonicalizeMapAndOperands(&lbMap, &lbOperands);

    fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubOperands, DI);
    affine::canonicalizeMapAndOperands(&ubMap, &ubOperands);

    forOp.setLowerBounds(lbOperands, lbMap);
    forOp.setUpperBounds(ubOperands, ubMap);
  }

  LogicalResult matchAndRewrite(scf::ParallelOp loop,
                                PatternRewriter &rewriter) const final {
    OpBuilder builder(loop);

    if (loop.getResults().size())
      return failure();

    if (!llvm::all_of(loop.getLowerBound(), isValidIndex)) {
      return failure();
    }

    if (!llvm::all_of(loop.getUpperBound(), isValidIndex)) {
      return failure();
    }

    SmallVector<int64_t> steps;
    for (auto step : loop.getStep())
      if (auto cst = step.getDefiningOp<ConstantIndexOp>())
        steps.push_back(cst.value());
      else
        return failure();

    ArrayRef<AtomicRMWKind> reductions;
    SmallVector<AffineMap> bounds;
    for (size_t i = 0; i < loop.getLowerBound().size(); i++)
      bounds.push_back(AffineMap::get(
          /*dimCount=*/0, /*symbolCount=*/loop.getLowerBound().size(),
          builder.getAffineSymbolExpr(i)));
    affine::AffineParallelOp affineLoop =
        rewriter.create<affine::AffineParallelOp>(
            loop.getLoc(), loop.getResultTypes(), reductions, bounds,
            loop.getLowerBound(), bounds, loop.getUpperBound(),
            steps); //, loop.getInitVals());

    canonicalizeLoopBounds(rewriter, affineLoop);

    auto mergedYieldOp =
        cast<scf::ReduceOp>(loop.getRegion().front().getTerminator());

    Block &newBlock = affineLoop.getRegion().front();

    // The terminator is added if the iterator args are not provided.
    // see the ::build method.
    if (affineLoop.getResults().size() == 0) {
      auto *affineYieldOp = newBlock.getTerminator();
      rewriter.eraseOp(affineYieldOp);
    }

    SmallVector<Value> vals;
    for (Value arg : affineLoop.getRegion().front().getArguments()) {
      vals.push_back(arg);
    }
    rewriter.mergeBlocks(&loop.getRegion().front(),
                         &affineLoop.getRegion().front(), vals);

    rewriter.setInsertionPoint(mergedYieldOp);
    rewriter.create<affine::AffineYieldOp>(mergedYieldOp.getLoc(),
                                           mergedYieldOp.getOperands());
    rewriter.eraseOp(mergedYieldOp);

    rewriter.replaceOp(loop, affineLoop.getResults());

    return success();
  }
};

void AffineCFGPass::runOnOperation() {
  mlir::RewritePatternSet rpl(getOperation()->getContext());
  rpl.add</*SimplfyIntegerCastMath, */ CanonicalizeAffineApply, ForOpRaising,
          ParallelOpRaising, CanonicalizeIndexCast<IndexCastOp>,
          CanonicalizeIndexCast<IndexCastUIOp>,
          /* IndexCastMovement,*/ AffineFixup<affine::AffineLoadOp>,
          AffineFixup<affine::AffineStoreOp>, CanonicalizIfBounds,
          MoveStoreToAffine, MoveIfToAffine, MoveLoadToAffine,
          CanonicalieForBounds>(getOperation()->getContext());
  GreedyRewriteConfig config;
  (void)applyPatternsAndFoldGreedily(getOperation(), std::move(rpl), config);
}

bool valueCmp(Cmp cmp, Value bval, ValueOrInt val) {
  if (auto icast = bval.getDefiningOp<IndexCastOp>()) {
    return valueCmp(cmp, icast.getIn(), val);
  }
  if (auto icast = bval.getDefiningOp<IndexCastUIOp>()) {
    return valueCmp(cmp, icast.getIn(), val);
  }

  IntegerAttr iattr;
  if (matchPattern(bval, m_Constant(&iattr))) {
    switch (cmp) {
    case Cmp::EQ:
      return val == iattr.getValue();
    case Cmp::LT:
      return val > iattr.getValue();
    case Cmp::LE:
      return val >= iattr.getValue();
    case Cmp::GT:
      return val < iattr.getValue();
    case Cmp::GE:
      return val <= iattr.getValue();
    }
  }

  if (cmp == Cmp::GE && !val.isValue && val.i_val == 0) {
    if (auto baval = bval.getDefiningOp<arith::AddIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val) &&
             valueCmp(cmp, baval.getRhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::ShRUIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::ShLIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::DivUIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
  }

  if (auto baval = dyn_cast<BlockArgument>(bval)) {
    if (affine::AffineForOp afFor =
            dyn_cast<affine::AffineForOp>(baval.getOwner()->getParentOp())) {
      auto for_lb = afFor.getLowerBoundMap().getResults()[baval.getArgNumber()];
      auto for_ub = afFor.getUpperBoundMap().getResults()[baval.getArgNumber()];
      switch (cmp) {
      // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
      case Cmp::EQ: {
        if (!valueCmp(Cmp::EQ, for_lb, afFor.getLowerBoundMap().getNumDims(),
                      afFor.getLowerBoundOperands(), val))
          return false;
        if (!val.isValue) {
          if (!valueCmp(Cmp::EQ, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val.i_val + 1))
            return false;
          return true;
        }
        return false;
      }
      // \forall i \in [LB, UB) < k   => UB <= k
      case Cmp::LT: {
        return valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val);
      }
      // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
      case Cmp::LE: {
        if (!val.isValue) {
          return valueCmp(Cmp::LE, for_ub,
                          afFor.getUpperBoundMap().getNumDims(),
                          afFor.getUpperBoundOperands(), val.i_val + 1);
        }
        return valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val);
      }
      // \forall i \in [LB, UB) > k   => LB > k
      case Cmp::GT: {
        return valueCmp(Cmp::GT, for_lb, afFor.getLowerBoundMap().getNumDims(),
                        afFor.getLowerBoundOperands(), val);
      }
      // \forall i \in [LB, UB) >= k   => LB >= k
      case Cmp::GE: {
        return valueCmp(Cmp::GE, for_lb, afFor.getLowerBoundMap().getNumDims(),
                        afFor.getLowerBoundOperands(), val);
      }
      }
    }
    if (affine::AffineParallelOp afFor = dyn_cast<affine::AffineParallelOp>(
            baval.getOwner()->getParentOp())) {
      switch (cmp) {
      // \forall i \in [max(LB...), min(UB...)) == k   => all(LB == k) and
      // all(UB == k+1)
      case Cmp::EQ: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (!valueCmp(Cmp::EQ, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                        afFor.getLowerBoundsOperands(), val))
            return false;
        if (!val.isValue) {
          for (auto for_ub :
               afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
            if (!valueCmp(Cmp::EQ, for_ub,
                          afFor.getUpperBoundsMap().getNumDims(),
                          afFor.getUpperBoundsOperands(), val.i_val + 1))
              return false;
          return true;
        }
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) < k   => any(UB <= k)
      case Cmp::LT: {
        for (auto for_ub :
             afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundsMap().getNumDims(),
                       afFor.getUpperBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) <= k   => any(UB-1 <= k)  =>
      // any(UB <= k+1)
      case Cmp::LE: {
        if (!val.isValue) {
          for (auto for_ub :
               afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
            if (valueCmp(Cmp::LE, for_ub,
                         afFor.getUpperBoundsMap().getNumDims(),
                         afFor.getUpperBoundsOperands(), val.i_val + 1))
              return true;
          return false;
        }

        for (auto for_ub :
             afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundsMap().getNumDims(),
                       afFor.getUpperBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) > k   => any(LB > k)
      case Cmp::GT: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::GT, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                       afFor.getLowerBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) >= k   => any(LB >= k)
      case Cmp::GE: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::GE, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                       afFor.getLowerBoundsOperands(), val))
            return true;
        return false;
      }
      }
    }

    if (scf::ForOp afFor =
            dyn_cast<scf::ForOp>(baval.getOwner()->getParentOp())) {
      if (baval.getArgNumber() == 0) {
        auto for_lb = afFor.getLowerBound();
        auto for_ub = afFor.getUpperBound();
        switch (cmp) {
        // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
        case Cmp::EQ: {
          if (!valueCmp(Cmp::EQ, for_lb, val))
            return false;
          if (!val.isValue) {
            if (!valueCmp(Cmp::EQ, for_ub, val.i_val + 1))
              return false;
            return true;
          }
          return false;
        }
        // \forall i \in [LB, UB) < k   => UB <= k
        case Cmp::LT: {
          return valueCmp(Cmp::LE, for_ub, val);
        }
        // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
        case Cmp::LE: {
          if (!val.isValue) {
            return valueCmp(Cmp::LE, for_ub, val.i_val + 1);
          }
          return valueCmp(Cmp::LE, for_ub, val);
        }
        // \forall i \in [LB, UB) > k   => LB > k
        case Cmp::GT: {
          return valueCmp(Cmp::GT, for_lb, val);
        }
        // \forall i \in [LB, UB) >= k   => LB >= k
        case Cmp::GE: {
          return valueCmp(Cmp::GE, for_lb, val);
        }
        }
      }
    }

    if (scf::ParallelOp afFor =
            dyn_cast<scf::ParallelOp>(baval.getOwner()->getParentOp())) {
      auto for_lb = afFor.getLowerBound()[baval.getArgNumber()];
      auto for_ub = afFor.getUpperBound()[baval.getArgNumber()];
      switch (cmp) {
      // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
      case Cmp::EQ: {
        if (!valueCmp(Cmp::EQ, for_lb, val))
          return false;
        if (!val.isValue) {
          if (!valueCmp(Cmp::EQ, for_ub, val.i_val + 1))
            return false;
          return true;
        }
        return false;
      }
      // \forall i \in [LB, UB) < k   => UB <= k
      case Cmp::LT: {
        return valueCmp(Cmp::LE, for_ub, val);
      }
      // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
      case Cmp::LE: {
        if (!val.isValue) {
          return valueCmp(Cmp::LE, for_ub, val.i_val + 1);
        }
        return valueCmp(Cmp::LE, for_ub, val);
      }
      // \forall i \in [LB, UB) > k   => LB > k
      case Cmp::GT: {
        return valueCmp(Cmp::GT, for_lb, val);
      }
      // \forall i \in [LB, UB) >= k   => LB >= k
      case Cmp::GE: {
        return valueCmp(Cmp::GE, for_lb, val);
      }
      }
    }
  }
  if (val.isValue && val.v_val == bval) {
    switch (cmp) {
    case Cmp::EQ:
      return true;
    case Cmp::LT:
      return false;
    case Cmp::LE:
      return true;
    case Cmp::GT:
      return false;
    case Cmp::GE:
      return true;
    }
  }
  return false;
}

bool valueCmp(Cmp cmp, AffineExpr expr, size_t numDim, ValueRange operands,
              ValueOrInt val) {

  if (auto opd = expr.dyn_cast<AffineConstantExpr>()) {
    switch (cmp) {
    case Cmp::EQ:
      return val == opd.getValue();
    case Cmp::LT:
      return val > opd.getValue();
    case Cmp::LE:
      return val >= opd.getValue();
    case Cmp::GT:
      return val < opd.getValue();
    case Cmp::GE:
      return val <= opd.getValue();
    }
  }
  if (auto opd = expr.dyn_cast<AffineDimExpr>()) {
    return valueCmp(cmp, operands[opd.getPosition()], val);
  }
  if (auto opd = expr.dyn_cast<AffineSymbolExpr>()) {
    return valueCmp(cmp, operands[opd.getPosition() + numDim], val);
  }

  if (auto bop = expr.dyn_cast<AffineBinaryOpExpr>()) {
    if (bop.getKind() == AffineExprKind::Add) {
      switch (cmp) {
      case Cmp::EQ:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::LT:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val)) ||
               (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val));
      case Cmp::LE:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::GT:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val)) ||
               (valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val));
      case Cmp::GE:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      }
    }
    if (bop.getKind() == AffineExprKind::Mul && val == 0) {
      switch (cmp) {
      case Cmp::EQ:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) ||
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::LT:
        return (valueCmp(Cmp::LT, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::GT, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GT, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::LT, bop.getRHS(), numDim, operands, val));
      case Cmp::LE:
        return valueCmp(Cmp::EQ, bop.getLHS(), numDim, operands, val) ||
               valueCmp(Cmp::EQ, bop.getRHS(), numDim, operands, val) ||
               ((valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val)) ||
                (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val)));
      case Cmp::GT:
        return (valueCmp(Cmp::LT, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::LT, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GT, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::GT, bop.getRHS(), numDim, operands, val));
      case Cmp::GE:
        return valueCmp(Cmp::EQ, bop.getLHS(), numDim, operands, val) ||
               valueCmp(Cmp::EQ, bop.getRHS(), numDim, operands, val) ||
               ((valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val)) ||
                (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val)));
      }
    }
  }
  return false;
}

bool isReadOnly(Operation *op) {
  bool hasRecursiveEffects = op->hasTrait<OpTrait::HasRecursiveMemoryEffects>();
  if (hasRecursiveEffects) {
    for (Region &region : op->getRegions()) {
      for (auto &block : region) {
        for (auto &nestedOp : block)
          if (!isReadOnly(&nestedOp))
            return false;
      }
    }
    return true;
  }

  // If the op has memory effects, try to characterize them to see if the op
  // is trivially dead here.
  if (auto effectInterface = dyn_cast<MemoryEffectOpInterface>(op)) {
    // Check to see if this op either has no effects, or only allocates/reads
    // memory.
    SmallVector<MemoryEffects::EffectInstance, 1> effects;
    effectInterface.getEffects(effects);
    if (!llvm::all_of(effects, [op](const MemoryEffects::EffectInstance &it) {
          return isa<MemoryEffects::Read>(it.getEffect());
        })) {
      return false;
    }
    return true;
  }
  return false;
}

bool isReadNone(Operation *op) {
  bool hasRecursiveEffects = op->hasTrait<OpTrait::HasRecursiveMemoryEffects>();
  if (hasRecursiveEffects) {
    for (Region &region : op->getRegions()) {
      for (auto &block : region) {
        for (auto &nestedOp : block)
          if (!isReadNone(&nestedOp))
            return false;
      }
    }
    return true;
  }

  // If the op has memory effects, try to characterize them to see if the op
  // is trivially dead here.
  if (auto effectInterface = dyn_cast<MemoryEffectOpInterface>(op)) {
    // Check to see if this op either has no effects, or only allocates/reads
    // memory.
    SmallVector<MemoryEffects::EffectInstance, 1> effects;
    effectInterface.getEffects(effects);
    if (llvm::any_of(effects, [op](const MemoryEffects::EffectInstance &it) {
          return isa<MemoryEffects::Read>(it.getEffect()) ||
                 isa<MemoryEffects::Write>(it.getEffect());
        })) {
      return false;
    }
    return true;
  }
  return false;
}
