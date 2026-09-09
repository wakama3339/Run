#pragma once
// Parser: tokens -> AST. Синтаксис v0.8:
//
//   fn main() void:
//       print("Hello World")
//       x: int = 42
//       const limit: int = 10
//       if x == 10:
//           print("ten")
//       elif x > 10:
//           print("big")
//       else:
//           print("small")
//       while x < 20:
//           x = x + 1
//       for i: int in 1..10:
//           print(i)
//       for j: int in 1..=3:
//           print(j)
//
// Блоки — двоеточием + отступом (INDENT/DEDENT из лексера).
// Тело также может быть одним стейтментом в той же строке: if a: print(1).
//
// Переменные объявляются как `x: type = value` (мутабельные),
// константы — как `const x: type = value` (присваивание запрещено).
// Присваивание — `x = value`. Выражения: полный приоритет
// `|| && == != < <= > >= + - * / унарные - ! ** (правоассоц.)`, скобки ().
#include "lexer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class TypeName { Void, Str, Int, Float, Bool, Char };

struct Expr {
    enum class Kind {
        StrLit, IntLit, FloatLit, BoolLit, CharLit, Var,
        Neg, Not,
        Add, Sub, Mul, Div, Mod, Pow,
        Eq, Ne, Lt, Le, Gt, Ge,
        And, Or, Call, Input
    } kind;
    std::string s;   // str literal / var name
    int64_t ival = 0;
    double fval = 0.0;
    bool bval = false;
    char cval = 0;
    std::unique_ptr<Expr> lhs, rhs; // Neg/Not: lhs; BinOp: lhs/rhs
    std::string callee;                            // Call
    std::vector<std::unique_ptr<Expr>> args;       // Call
    int line = 1;
};

struct Stmt {
    enum class Kind { Let, Const, Assign, Print, If, While, For, Return, Expr, Break, Continue } kind;
    std::string name;          // Let/Const/Assign
    bool hasType = false;
    TypeName type = TypeName::Void; // Let/Const
    std::unique_ptr<Expr> expr;     // значение (Let/Const/Assign/Print)
    int line = 1;
    // If / While: cond + thenBranch. For: varName/varType/lo/hi/inclusive + thenBranch.
    // Return: expr (null = голый return). Expr: вызов как стейтмент.
    std::unique_ptr<Expr> cond;
    std::vector<Stmt> thenBranch;
    struct Elif {
        std::unique_ptr<Expr> cond;
        std::vector<Stmt> body;
    };
    std::vector<Elif> elifs;
    std::unique_ptr<std::vector<Stmt>> elseBranch; // If: null = нет else
    std::string varName;          // For
    TypeName varType = TypeName::Void; // For (требуется int)
    std::unique_ptr<Expr> lo, hi; // For: границы (int, вычисляются один раз)
    bool inclusive = false;       // For: true для ..=
};

struct Func {
    struct Param {
        std::string name;
        TypeName type = TypeName::Void;
    };
    std::string name;
    std::vector<Param> params;
    TypeName ret = TypeName::Void;
    std::vector<Stmt> body;
    int line = 1;
    std::string file; // исходник (для ошибок при импортах)
};

struct Program {
    std::vector<std::string> imports; // пути из import "..." (как написаны)
    std::vector<Func> funcs;
};

TypeName typeFromName(const std::string& n, int line);
std::string typeName(TypeName t);

Program parse(const std::vector<Token>& toks, const std::string& fname);
