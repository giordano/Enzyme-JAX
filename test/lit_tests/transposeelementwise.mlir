// RUN: enzymexlamlir-opt --enzyme-hlo-generate-td="patterns=transpose_elementwise" --transform-interpreter --enzyme-hlo-remove-transform %s | FileCheck %s

module {
  func.func @main(%a : tensor<100x200x300xbf16>, %b: tensor<100x200x300xbf16>) -> tensor<300x100x200xbf16> {
    %1909 = stablehlo.subtract %a, %b : tensor<100x200x300xbf16>
    %1910 = stablehlo.transpose %1909, dims = [2, 0, 1] : (tensor<100x200x300xbf16>) -> tensor<300x100x200xbf16> 
    return %1910 : tensor<300x100x200xbf16> 
  }
}

// CHECK:  func.func @main(%arg0: tensor<100x200x300xbf16>, %arg1: tensor<100x200x300xbf16>) -> tensor<300x100x200xbf16> {
// CHECK-NEXT:    %0 = stablehlo.transpose %arg0, dims = [2, 0, 1] : (tensor<100x200x300xbf16>) -> tensor<300x100x200xbf16>
// CHECK-NEXT:    %1 = stablehlo.transpose %arg1, dims = [2, 0, 1] : (tensor<100x200x300xbf16>) -> tensor<300x100x200xbf16>
// CHECK-NEXT:    %2 = stablehlo.subtract %0, %1 : tensor<300x100x200xbf16>
// CHECK-NEXT:    return %2 : tensor<300x100x200xbf16>
// CHECK-NEXT:  }