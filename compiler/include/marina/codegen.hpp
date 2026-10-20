// LLVM IR (textual) generation from the checked IR.
#pragma once

#include <string>

#include <marina/checker.hpp>

namespace marina::codegen {

// Generate textual LLVM IR for a checked program. The IR calls the stable C
// ABI exported by libmarina_runtime; `clang` compiles and links it.
std::string generate(const ir::Checked& c);

} // namespace marina::codegen
