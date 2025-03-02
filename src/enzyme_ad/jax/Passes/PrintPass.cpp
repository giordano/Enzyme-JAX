//===- PrintPass.cpp - Print the MLIR module                     ------------ //
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to print the MLIR module
//===----------------------------------------------------------------------===//

#include "src/enzyme_ad/jax/Passes/Passes.h"

namespace mlir {
namespace enzyme {
#define GEN_PASS_DEF_PRINTPASS
#include "src/enzyme_ad/jax/Passes/Passes.h.inc"
} // namespace enzyme
} // namespace mlir

using namespace mlir;
using namespace mlir::enzyme;

namespace {
struct PrintPass : public enzyme::impl::PrintPassBase<PrintPass> {
  using PrintPassBase::PrintPassBase;

  void runOnOperation() override {

    OpPrintingFlags flags;
    if (debug)
      flags.enableDebugInfo(/*pretty*/ false);
    if (use_stdout) {
      getOperation()->print(llvm::outs(), flags);
      llvm::outs() << "\n";
    } else {
      getOperation()->print(llvm::errs(), flags);
      llvm::errs() << "\n";
    }
  }
};

} // end anonymous namespace
