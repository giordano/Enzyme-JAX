// RUN: enzymexlamlir-opt %s --pass-pipeline="builtin.module(lower-kernel{backend=cpu},canonicalize)" | FileCheck %s

module {
  llvm.func internal unnamed_addr fastcc @throw_boundserror_2676() attributes {dso_local, no_inline, sym_visibility = "private"} {
    llvm.unreachable
  }
  llvm.func internal ptx_kernelcc @kern(%arg0: !llvm.ptr<1>) {
    %0 = llvm.mlir.constant(63 : i32) : i32
    %1 = nvvm.read.ptx.sreg.tid.x : i32
    %2 = llvm.icmp "ugt" %1, %0 : i32
    llvm.cond_br %2, ^bb2, ^bb1
  ^bb1:  // pred: ^bb0
    %4 = llvm.zext %1 : i32 to i64
    %5 = llvm.getelementptr inbounds %arg0[%4] : (!llvm.ptr<1>, i64) -> !llvm.ptr<1>, i64
    %6 = llvm.load %5 {alignment = 1 : i64} : !llvm.ptr<1> -> i64
    %7 = llvm.mul %6, %6 : i64
    llvm.store %7, %5 {alignment = 1 : i64} : i64, !llvm.ptr<1>
    llvm.return
  ^bb2:  // pred: ^bb0
    llvm.call fastcc @throw_boundserror_2676() : () -> ()
    llvm.unreachable
  }
  func.func @main(%arg0: tensor<64xi64>) -> tensor<64xi64> {
    %c0 = stablehlo.constant dense<0> : tensor<i64>
    %c1 = stablehlo.constant dense<1> : tensor<i64>
    %c40 = stablehlo.constant dense<40> : tensor<i64>
    %0 = enzymexla.kernel_call @kern blocks in (%c1, %c1, %c1) threads in (%c1, %c1, %c40) shmem=%c0 (%arg0) {output_operand_aliases = [#stablehlo.output_operand_alias<output_tuple_indices = [], operand_index = 0, operand_tuple_indices = []>]} : (tensor<64xi64>) -> tensor<64xi64>
    return %0 : tensor<64xi64>
  }
}

// CHECK:  func.func private @kern$par0(%arg0: !llvm.ptr<1>) {
// CHECK-NEXT:    %c0_i32 = arith.constant 0 : i32
// CHECK-NEXT:    %0 = llvm.mlir.constant(63 : i32) : i32
// CHECK-NEXT:    %c0 = arith.constant 0 : index
// CHECK-NEXT:    %c1 = arith.constant 1 : index
// CHECK-NEXT:    %c40 = arith.constant 40 : index
// CHECK-NEXT:    scf.parallel (%arg1) = (%c0) to (%c40) step (%c1) {
// CHECK-NEXT:      scf.execute_region {
// CHECK-NEXT:        %1 = llvm.icmp "ugt" %c0_i32, %0 : i32
// CHECK-NEXT:        llvm.cond_br %1, ^bb2, ^bb1
// CHECK-NEXT:      ^bb1:  // pred: ^bb0
// CHECK-NEXT:        %2 = llvm.load %arg0 {alignment = 1 : i64} : !llvm.ptr<1> -> i64
// CHECK-NEXT:        %3 = llvm.mul %2, %2 : i64
// CHECK-NEXT:        llvm.store %3, %arg0 {alignment = 1 : i64} : i64, !llvm.ptr<1>
// CHECK-NEXT:        scf.yield
// CHECK-NEXT:      ^bb2:  // pred: ^bb0
// CHECK-NEXT:        llvm.call fastcc @throw_boundserror_2676() : () -> ()
// CHECK-NEXT:        scf.yield
// CHECK-NEXT:      }
// CHECK-NEXT:      scf.reduce
// CHECK-NEXT:    }
// CHECK-NEXT:    return
// CHECK-NEXT:  }

// CHECK:  func.func @main(%arg0: tensor<64xi64>) -> tensor<64xi64> {
// CHECK-NEXT:    %0 = enzymexla.jit_call @kern$par0 (%arg0) {output_operand_aliases = [#stablehlo.output_operand_alias<output_tuple_indices = [], operand_index = 0, operand_tuple_indices = []>]} : (tensor<64xi64>) -> tensor<64xi64>
// CHECK-NEXT:    return %0 : tensor<64xi64>
// CHECK-NEXT:  }
