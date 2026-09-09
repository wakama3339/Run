#pragma once
// IR: промежуточное представление. Архитектура для развития:
// Module -> Function(params, ret) -> BasicBlock -> Instr.
// CondBr/Br/Call/Ret — полный контроль потока и вызовы.
// Позже: новые типы, массивы, оптимизационные проходы.
#include "parser.h"
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct LoopCtx {
    std::string breakLabel;    // выход из цикла
    std::string continueLabel; // while: проверка; for: блок инкремента
};
#include <vector>

enum class IRType { Void, Str, Int, Float, Bool, Char };

IRType irTypeFromName(TypeName t);
std::string irTypeName(IRType t);

struct IRInstr {
    enum class Op {
        ConstStr,   // dst = const str (strId)
        ConstInt,   // dst = const int
        ConstFloat, // dst = const double
        ConstBool,  // dst = const bool
        ConstChar,  // dst = const char
        Copy,       // dst = src
        Add, Sub, Mul, Div, Neg, // dst = a op b (Neg: a); int/float
        Mod,        // dst = a % b; только int (остаток знакового деления)
        Pow,        // dst = a ** b (b — всегда int; type = тип a)
        Eq, Ne, Lt, Le, Gt, Ge,  // dst: bool = a cmp b
        And, Or,    // dst: bool = a/b (bool); eager (выражения чистые)
        Not,        // dst: bool = !a
        Br,         // безусловный переход: a = метка блока
        CondBr,     // if src != 0: a(true-блок) else b(false-блок)
        Call,       // dst = callee(args...); type = тип возврата (dst пуст для void)
        Input,      // dst = input([src]); src пуст = без промпта; type = Str
        Print,      // print(src) — тип берётся из src
        Ret,        // return [src]; type = тип возврата функции
    } op = Op::Ret;

    std::string dst, src, a, b; // имена SSA-темпов / переменных; a/b: операнды или метки
    std::string callee;                  // Call
    std::vector<std::string> args;       // Call: имена аргументов
    std::vector<IRType> argTypes;        // Call: типы аргументов (для ABI)
    IRType type = IRType::Void;  // тип dst/src/возврата
    // константы:
    int strId = -1;
    int64_t ival = 0;
    double fval = 0.0;
    bool bval = false;
    char cval = 0;
};

struct IRBlock {
    std::string label = "entry";
    std::vector<IRInstr> instrs;
};

struct IRFunction {
    std::string name;
    std::vector<std::pair<std::string, IRType>> params; // порядок значим (ABI)
    IRType ret = IRType::Void;
    std::map<std::string, IRType> locals; // переменные + темпы (включая params)
    std::set<std::string> consts;         // подмножество locals: только чтение
    std::vector<IRBlock> blocks;
};

struct IRModule {
    std::vector<std::string> strings; // пул строк (ConstStr ссылается по id)
    std::vector<IRFunction> funcs;
    int addString(const std::string& s); // intern
    std::string dump() const;            // --emit-ir
};

struct IRBuildError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// AST -> IR (с проверкой типов). Возвращает модуль.
IRModule buildIR(const Program& prog);
