#pragma once
// Lexer: source text -> tokens. No dependencies.
#include <cstdint>
#include <string>
#include <vector>

enum class TokKind {
    Fn, Const, If, Elif, Else, While, For, In, Return, Import, Break, Continue,
    True, False,
    Ident,
    IntLit, FloatLit, StringLit, CharLit,
    LParen, RParen, Colon, Comma, Assign,
    Plus, Minus, Star, Slash, StarStar, Mod,
    EqEq, NotEq, Lt, LtEq, Gt, GtEq, Bang,
    AmpAmp, PipePipe, DotDot, DotDotEq,
    Newline, Indent, Dedent, Eof,
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;      // raw text / ident / string content
    int64_t ival = 0;      // int literal
    double fval = 0.0;     // float literal
    char cval = 0;         // char literal
    int line = 1, col = 1;
};

std::vector<Token> lex(const std::string& src, const std::string& fname);
std::string tokKindName(TokKind k);
