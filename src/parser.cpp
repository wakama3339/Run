#include "parser.h"
#include <stdexcept>

std::string typeName(TypeName t) {
    switch (t) {
        case TypeName::Void: return "void";
        case TypeName::Str: return "str";
        case TypeName::Int: return "int";
        case TypeName::Float: return "float";
        case TypeName::Bool: return "bool";
        case TypeName::Char: return "char";
    }
    return "?";
}

TypeName typeFromName(const std::string& n, int line) {
    if (n == "void") return TypeName::Void;
    if (n == "str") return TypeName::Str;
    if (n == "int") return TypeName::Int;
    if (n == "float") return TypeName::Float;
    if (n == "bool") return TypeName::Bool;
    if (n == "char") return TypeName::Char;
    throw std::runtime_error("parse error at line " + std::to_string(line) +
                             ": unknown type '" + n + "'");
}

// Встроенные имена: print(...) — стейтмент, input(...) — выражение.
// Объявлять так переменные/функции/параметры нельзя.
static bool isReserved(const std::string& w) {
    return w == "print" || w == "input";
}

static std::string reservedMsg(const std::string& w, int line) {
    return "parse error at line " + std::to_string(line) + ": '" + w + "' is reserved";
}

struct P {
    const std::vector<Token>& t;
    size_t i = 0;
    std::string fname;
    const Token& pk() { return t[i]; }
    const Token& nx() { if (i + 1 < t.size()) return t[i + 1]; return t.back(); }
    bool at(TokKind k) { return pk().kind == k; }
    Token eat(TokKind k, const std::string& what) {
        if (pk().kind != k)
            throw std::runtime_error("parse error at line " + std::to_string(pk().line) +
                                     ": expected " + what + ", got " + tokKindName(pk().kind) +
                                     " '" + pk().text + "'");
        return t[i++];
    }
    void skipNl() { while (at(TokKind::Newline)) i++; }

    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();
    std::unique_ptr<Expr> parseEq();
    std::unique_ptr<Expr> parseCmp();
    std::unique_ptr<Expr> parseAdd();
    std::unique_ptr<Expr> parseMul();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePow();
    std::unique_ptr<Expr> parsePrimary();

    Stmt parseOneStmt();
    Stmt parseIfStmt();
    Stmt parseWhileStmt();
    Stmt parseForStmt();
    std::vector<Stmt> parseBody(); // ':' (NEWLINE INDENT stmts DEDENT | один стейтмент)
};

std::unique_ptr<Expr> P::parsePrimary() {
    const Token& c = pk();
    if (c.kind == TokKind::StringLit) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::StrLit; e->s = c.text; e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::IntLit) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::IntLit; e->ival = c.ival; e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::FloatLit) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::FloatLit; e->fval = c.fval; e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::True || c.kind == TokKind::False) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::BoolLit; e->bval = (c.kind == TokKind::True);
        e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::CharLit) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::CharLit; e->cval = c.cval; e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::Ident) {
        // вызов f(...) как выражение; print(...) — только стейтмент;
        // input(...) — встроенный вызов (0 или 1 str-аргумент)
        if (c.text == "print" && nx().kind == TokKind::LParen)
            throw std::runtime_error("parse error at line " + std::to_string(c.line) +
                                     ": print(...) is a statement and returns nothing");
        if (c.text == "input" && nx().kind == TokKind::LParen) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Input; e->line = c.line;
            i += 2; // input (
            if (!at(TokKind::RParen)) {
                while (true) {
                    e->args.push_back(parseExpr());
                    if (e->args.size() > 1)
                        throw std::runtime_error("parse error at line " +
                                                 std::to_string(c.line) +
                                                 ": input() takes 0 or 1 argument");
                    if (at(TokKind::Comma)) { i++; continue; }
                    break;
                }
            }
            eat(TokKind::RParen, "')'");
            return e;
        }
        if (nx().kind == TokKind::LParen) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Kind::Call; e->callee = c.text; e->line = c.line;
            i += 2; // name (
            if (!at(TokKind::RParen)) {
                while (true) {
                    e->args.push_back(parseExpr());
                    if (at(TokKind::Comma)) { i++; continue; }
                    break;
                }
            }
            eat(TokKind::RParen, "')'");
            return e;
        }
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Var; e->s = c.text; e->line = c.line; i++;
        return e;
    }
    if (c.kind == TokKind::LParen) {
        i++;
        auto e = parseExpr();
        eat(TokKind::RParen, "')'");
        return e;
    }
    throw std::runtime_error("parse error at line " + std::to_string(c.line) +
                             ": unexpected " + tokKindName(c.kind) + " '" + c.text +
                             "' in expression");
}

std::unique_ptr<Expr> P::parseUnary() {
    if (at(TokKind::Minus)) {
        int ln = pk().line; i++;
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Neg; e->lhs = parseUnary(); e->line = ln;
        return e;
    }
    if (at(TokKind::Bang)) {
        int ln = pk().line; i++;
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Not; e->lhs = parseUnary(); e->line = ln;
        return e;
    }
    return parsePow();
}

// ** — правоассоциитивен и выше унарных (как в Python: -2**2 == -(2**2),
// показатель допускает унарный минус: 2**-1.
std::unique_ptr<Expr> P::parsePow() {
    auto base = parsePrimary();
    if (at(TokKind::StarStar)) {
        int ln = pk().line; i++;
        auto exp = parseUnary();
        auto n = std::make_unique<Expr>();
        n->kind = Expr::Kind::Pow;
        n->lhs = std::move(base); n->rhs = std::move(exp); n->line = ln;
        return n;
    }
    return base;
}

std::unique_ptr<Expr> P::parseMul() {
    auto e = parseUnary();
    while (at(TokKind::Star) || at(TokKind::Slash) || at(TokKind::Mod)) {
        TokKind k = pk().kind;
        int ln = pk().line; i++;
        auto r = parseUnary();
        auto n = std::make_unique<Expr>();
        if (k == TokKind::Star) n->kind = Expr::Kind::Mul;
        else if (k == TokKind::Slash) n->kind = Expr::Kind::Div;
        else n->kind = Expr::Kind::Mod;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseAdd() {
    auto e = parseMul();
    while (at(TokKind::Plus) || at(TokKind::Minus)) {
        bool isAdd = at(TokKind::Plus);
        int ln = pk().line; i++;
        auto r = parseMul();
        auto n = std::make_unique<Expr>();
        n->kind = isAdd ? Expr::Kind::Add : Expr::Kind::Sub;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseCmp() {
    auto e = parseAdd();
    while (at(TokKind::Lt) || at(TokKind::LtEq) || at(TokKind::Gt) || at(TokKind::GtEq)) {
        TokKind k = pk().kind;
        int ln = pk().line; i++;
        auto r = parseAdd();
        auto n = std::make_unique<Expr>();
        if (k == TokKind::Lt) n->kind = Expr::Kind::Lt;
        else if (k == TokKind::LtEq) n->kind = Expr::Kind::Le;
        else if (k == TokKind::Gt) n->kind = Expr::Kind::Gt;
        else n->kind = Expr::Kind::Ge;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseEq() {
    auto e = parseCmp();
    while (at(TokKind::EqEq) || at(TokKind::NotEq)) {
        bool isEq = at(TokKind::EqEq);
        int ln = pk().line; i++;
        auto r = parseCmp();
        auto n = std::make_unique<Expr>();
        n->kind = isEq ? Expr::Kind::Eq : Expr::Kind::Ne;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseAnd() {
    auto e = parseEq();
    while (at(TokKind::AmpAmp)) {
        int ln = pk().line; i++;
        auto r = parseEq();
        auto n = std::make_unique<Expr>();
        n->kind = Expr::Kind::And;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseOr() {
    auto e = parseAnd();
    while (at(TokKind::PipePipe)) {
        int ln = pk().line; i++;
        auto r = parseAnd();
        auto n = std::make_unique<Expr>();
        n->kind = Expr::Kind::Or;
        n->lhs = std::move(e); n->rhs = std::move(r); n->line = ln;
        e = std::move(n);
    }
    return e;
}

std::unique_ptr<Expr> P::parseExpr() { return parseOr(); }

// Тело после ':': либо один стейтмент в той же строке,
// либо NEWLINE INDENT ... DEDENT.
std::vector<Stmt> P::parseBody() {
    std::vector<Stmt> out;
    if (at(TokKind::Newline)) {
        i++;
        eat(TokKind::Indent, "indented block after ':'");
        while (!at(TokKind::Dedent)) {
            if (at(TokKind::Eof))
                throw std::runtime_error("parse error: expected dedent before end of file");
            out.push_back(parseOneStmt());
            if (at(TokKind::Newline)) i++;
        }
        i++; // DEDENT
        return out;
    }
    out.push_back(parseOneStmt());
    return out;
}

Stmt P::parseIfStmt() {
    Token kw = eat(TokKind::If, "'if'");
    Stmt s; s.kind = Stmt::Kind::If; s.line = kw.line;
    s.cond = parseExpr();
    eat(TokKind::Colon, "':'");
    s.thenBranch = parseBody();
    skipNl();
    while (at(TokKind::Elif)) {
        i++;
        Stmt::Elif e;
        e.cond = parseExpr();
        eat(TokKind::Colon, "':'");
        e.body = parseBody();
        s.elifs.push_back(std::move(e));
        skipNl();
    }
    if (at(TokKind::Else)) {
        i++;
        eat(TokKind::Colon, "':'");
        s.elseBranch = std::make_unique<std::vector<Stmt>>(parseBody());
        skipNl();
    }
    return s;
}

Stmt P::parseWhileStmt() {
    Token kw = eat(TokKind::While, "'while'");
    Stmt s; s.kind = Stmt::Kind::While; s.line = kw.line;
    s.cond = parseExpr();
    eat(TokKind::Colon, "':'");
    s.thenBranch = parseBody();
    return s;
}

Stmt P::parseForStmt() {
    // for x: int in lo..hi: ...  |  for x: int in lo..=hi: ...
    Token kw = eat(TokKind::For, "'for'");
    Stmt s; s.kind = Stmt::Kind::For; s.line = kw.line;
    Token vn = eat(TokKind::Ident, "loop variable name");
    if (isReserved(vn.text)) throw std::runtime_error(reservedMsg(vn.text, vn.line));
    s.varName = vn.text;
    eat(TokKind::Colon, "':'");
    Token tn = eat(TokKind::Ident, "type");
    s.varType = typeFromName(tn.text, tn.line);
    eat(TokKind::In, "'in'");
    s.lo = parseExpr();
    if (at(TokKind::DotDotEq)) {
        s.inclusive = true; i++;
    } else {
        eat(TokKind::DotDot, "'..'");
    }
    s.hi = parseExpr();
    eat(TokKind::Colon, "':'");
    s.thenBranch = parseBody();
    return s;
}

static Stmt parseOneStmtImpl(P& p) {
    const Token& c = p.pk();
    if (c.kind == TokKind::If) {
        return p.parseIfStmt();
    }
    if (c.kind == TokKind::While) {
        return p.parseWhileStmt();
    }
    if (c.kind == TokKind::For) {
        return p.parseForStmt();
    }
    if (c.kind == TokKind::Elif || c.kind == TokKind::Else) {
        throw std::runtime_error("parse error at line " + std::to_string(c.line) + ": '" +
                                 tokKindName(c.kind) + "' without 'if'");
    }
    if (c.kind == TokKind::Import) {
        throw std::runtime_error("parse error at line " + std::to_string(c.line) +
                                 ": 'import' is only allowed at top level");
    }
    if (c.kind == TokKind::Break) {
        Stmt s; s.kind = Stmt::Kind::Break; s.line = c.line; p.i++;
        return s;
    }
    if (c.kind == TokKind::Continue) {
        Stmt s; s.kind = Stmt::Kind::Continue; s.line = c.line; p.i++;
        return s;
    }
    if (c.kind == TokKind::Indent || c.kind == TokKind::Dedent) {
        throw std::runtime_error("parse error at line " + std::to_string(c.line) +
                                 ": unexpected " + tokKindName(c.kind));
    }
    if (c.kind == TokKind::Return) {
        // return expr | return (голый — только для void)
        int ln = c.line; p.i++;
        Stmt s; s.kind = Stmt::Kind::Return; s.line = ln;
        if (p.at(TokKind::Newline) || p.at(TokKind::Dedent) || p.at(TokKind::Eof)) {
            s.expr = nullptr;
        } else {
            s.expr = p.parseExpr();
        }
        return s;
    }
    // дальше — общие ветки Const/Ident (используют c из шапки выше)
            if (c.kind == TokKind::Const) {
                // константа: const x: type = value
                int ln = c.line; p.i++;
                Token vn = p.eat(TokKind::Ident, "constant name");
                if (isReserved(vn.text)) throw std::runtime_error(reservedMsg(vn.text, ln));
                Stmt s; s.kind = Stmt::Kind::Const; s.name = vn.text; s.line = ln;
                p.eat(TokKind::Colon, "':'");
                Token tn = p.eat(TokKind::Ident, "type");
                s.hasType = true;
                s.type = typeFromName(tn.text, tn.line);
                p.eat(TokKind::Assign, "'='");
                s.expr = p.parseExpr();
                return s;
            } else if (c.kind == TokKind::Ident) {
                std::string w = c.text;
                int ln = c.line;
                if (w == "print" && p.nx().kind == TokKind::LParen) {
                    p.i += 2; // print (
                    Stmt s; s.kind = Stmt::Kind::Print; s.line = ln;
                    s.expr = p.parseExpr();
                    p.eat(TokKind::RParen, "')'");
                    return s;
                } else if (p.nx().kind == TokKind::Colon) {
                    // объявление: x: type = value
                    if (isReserved(w)) throw std::runtime_error(reservedMsg(w, ln));
                    p.i += 2; // name :
                    Token tn = p.eat(TokKind::Ident, "type");
                    Stmt s; s.kind = Stmt::Kind::Let; s.name = w; s.line = ln;
                    s.hasType = true;
                    s.type = typeFromName(tn.text, tn.line);
                    p.eat(TokKind::Assign, "'='");
                    s.expr = p.parseExpr();
                    return s;
                } else if (p.nx().kind == TokKind::Assign) {
                    // присваивание: x = value
                    p.i += 2;
                    Stmt s; s.kind = Stmt::Kind::Assign; s.name = w; s.line = ln;
                    s.expr = p.parseExpr();
                    return s;
                } else if (p.nx().kind == TokKind::LParen) {
                    // вызов как стейтмент: foo(1, 2)
                    if (w == "input")
                        throw std::runtime_error(
                            "parse error at line " + std::to_string(ln) +
                            ": input() returns a string, use it in an expression " +
                            "(e.g. x: str = input())");
                    p.i += 2; // name (
                    auto e = std::make_unique<Expr>();
                    e->kind = Expr::Kind::Call; e->callee = w; e->line = ln;
                    if (!p.at(TokKind::RParen)) {
                        while (true) {
                            e->args.push_back(p.parseExpr());
                            if (p.at(TokKind::Comma)) { p.i++; continue; }
                            break;
                        }
                    }
                    p.eat(TokKind::RParen, "')'");
                    Stmt s; s.kind = Stmt::Kind::Expr; s.line = ln;
                    s.expr = std::move(e);
                    return s;
                } else {
                    throw std::runtime_error(
                        "parse error at line " + std::to_string(ln) +
                        ": unexpected '" + w +
                        "' (v0.8: x: type = ..., const ..., x = ..., print(...), if/while/for, break/continue)");
                }
            } else {
                throw std::runtime_error(
                    "parse error at line " + std::to_string(c.line) +
                    ": unexpected " + tokKindName(c.kind));
            }
}

Stmt P::parseOneStmt() { return parseOneStmtImpl(*this); }

Program parse(const std::vector<Token>& toks, const std::string& fname) {
    P p{toks, 0, fname};
    Program prog;
    p.skipNl();
    while (!p.at(TokKind::Eof)) {
        if (p.at(TokKind::Import)) {
            // import "path.rn" — только верхний уровень
            int ln = p.pk().line; p.i++;
            Token pt = p.eat(TokKind::StringLit, "import path string");
            if (pt.text.empty())
                throw std::runtime_error("parse error at line " + std::to_string(ln) +
                                         ": empty import path");
            prog.imports.push_back(pt.text);
            p.skipNl();
            continue;
        }
        p.eat(TokKind::Fn, "'fn' or 'import'");
        Token nm = p.eat(TokKind::Ident, "function name");
        if (isReserved(nm.text)) throw std::runtime_error(reservedMsg(nm.text, nm.line));
        p.eat(TokKind::LParen, "'('");
        Func f;
        f.name = nm.text; f.line = nm.line; f.file = fname;
        if (!p.at(TokKind::RParen)) {
            while (true) {
                Token pn = p.eat(TokKind::Ident, "parameter name");
                if (isReserved(pn.text)) throw std::runtime_error(reservedMsg(pn.text, pn.line));
                p.eat(TokKind::Colon, "':'");
                Token pt = p.eat(TokKind::Ident, "parameter type");
                Func::Param pr;
                pr.name = pn.text;
                pr.type = typeFromName(pt.text, pt.line);
                if (pr.type == TypeName::Void)
                    throw std::runtime_error("parse error at line " + std::to_string(pt.line) +
                                             ": parameter cannot be void");
                f.params.push_back(pr);
                if (p.at(TokKind::Comma)) { p.i++; continue; }
                break;
            }
        }
        p.eat(TokKind::RParen, "')'");
        Token rt = p.eat(TokKind::Ident, "return type");
        f.ret = typeFromName(rt.text, rt.line);
        p.eat(TokKind::Colon, "':'");
        f.body = p.parseBody();
        prog.funcs.push_back(std::move(f));
        p.skipNl();
    }
    // Пустая программа допустима (например, stdlib/builtins.rn v0.4 —
    // только комментарии + intrinsic print). Проверку "ровно один main"
    // делает buildIR.
    return prog;
}
