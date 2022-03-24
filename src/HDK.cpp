#include "HDK.h"

#include <iostream>

#include "AST/AST.h"
#include "MLIRGen.h"

#include "mlir/Dialect.h"

void mlir_test() {
  std::cout << "### Testing MLIR Dialect ###" << std::endl;

  hdk::AST::KernelSequence sequence;
  auto kernel = hdk::AST::Kernel();
  sequence.emplace_back(kernel);

  mlir::MLIRContext context;
  // Load our Dialect in this MLIR Context.
  context.getOrLoadDialect<hdk::HDKDialect>();
  auto module = hdk::mlirGen(context, sequence);
  if (!module) {
    std::cerr << "Failed to generate MLIR module!" << std::endl;
  } else {
    module->dump();
  }
}