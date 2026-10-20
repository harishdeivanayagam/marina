// Recursive-descent parser for the marina language.
#pragma once

#include <string>
#include <vector>

#include <marina/ast.hpp>
#include <marina/lexer.hpp>

namespace marina::ast {

struct ParseError {
    std::string msg;
    Span span;
};

Program parse(std::vector<Token> tokens);

} // namespace marina::ast
