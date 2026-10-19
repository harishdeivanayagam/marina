//! marina compiler library: lexer, parser, type checker and LLVM IR codegen.
//! Shared by the `marina` CLI and the `marina-lsp` language server.

pub mod ast;
pub mod checker;
pub mod codegen;
pub mod lexer;
pub mod parser;
pub mod pipeline;