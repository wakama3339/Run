#include "ir.h"
#include <sstream>

IRType irTypeFromName(TypeName t) {
    switch (t) {
        case TypeName::Void: return IRType::Void;
        case TypeName::Str: return IRType::Str;
        case TypeName::Int: return IRType::Int;
        case TypeName::Float: return IRType::Float;
        case TypeName::Bool: return IRType::Bool;
        case TypeName::Char: return IRType::Char;
    }
    return IRType::Void;
}

std::string irTypeName(IRType t) {
    switch (t) {
        case IRType::Void: return "void";
        case IRType::Str: return "str";
        case IRType::Int: return "int";
        case IRType::Float: return "float";
        case IRType::Bool: return "bool";
        case IRType::Char: return "char";
    }
    return "?";
}

int IRModule::addString(const std::string& s) {
    for (size_t i = 0; i < strings.size(); i++)
        if (strings[i] == s) return (int)i;
    strings.push_back(s);
    return (int)strings.size() - 1;
}

std::string IRModule::dump() const {
    std::ostringstream o;
    for (size_t i = 0; i < strings.size(); i++)
        o << "str@" << i << " = \"" << strings[i] << "\"\n";
    for (auto& f : funcs) {
        o << "fn " << f.name << "(";
        for (size_t i = 0; i < f.params.size(); i++) {
            if (i) o << ", ";
            o << "%" << f.params[i].first << " : " << irTypeName(f.params[i].second);
        }
        o << ") " << irTypeName(f.ret) << ":\n";
        for (auto& kv : f.locals)
            o << "  " << (f.consts.count(kv.first) ? "const %" : "local %") << kv.first
              << " : " << irTypeName(kv.second) << "\n";
        for (auto& b : f.blocks) {
            o << b.label << ":\n";
            for (auto& in : b.instrs) {
                o << "  ";
                switch (in.op) {
                    case IRInstr::Op::ConstStr:
                        o << "%" << in.dst << " = const str str@" << in.strId; break;
                    case IRInstr::Op::ConstInt:
                        o << "%" << in.dst << " = const int " << in.ival; break;
                    case IRInstr::Op::ConstFloat:
                        o << "%" << in.dst << " = const float " << in.fval; break;
                    case IRInstr::Op::ConstBool:
                        o << "%" << in.dst << " = const bool "
                          << (in.bval ? "true" : "false"); break;
                    case IRInstr::Op::ConstChar:
                        o << "%" << in.dst << " = const char '" << in.cval << "'"; break;
                    case IRInstr::Op::Copy:
                        o << "%" << in.dst << " = copy %" << in.src; break;
                    case IRInstr::Op::Add: o << "%" << in.dst << " = add %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Sub: o << "%" << in.dst << " = sub %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Mul: o << "%" << in.dst << " = mul %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Div: o << "%" << in.dst << " = div %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Mod: o << "%" << in.dst << " = mod %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Neg: o << "%" << in.dst << " = neg %" << in.a; break;
                    case IRInstr::Op::Pow: o << "%" << in.dst << " = pow %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Eq: o << "%" << in.dst << " = eq %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Ne: o << "%" << in.dst << " = ne %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Lt: o << "%" << in.dst << " = lt %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Le: o << "%" << in.dst << " = le %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Gt: o << "%" << in.dst << " = gt %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Ge: o << "%" << in.dst << " = ge %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::And: o << "%" << in.dst << " = and %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Or: o << "%" << in.dst << " = or %" << in.a << ", %" << in.b; break;
                    case IRInstr::Op::Not: o << "%" << in.dst << " = not %" << in.a; break;
                    case IRInstr::Op::Br: o << "br " << in.a; break;
                    case IRInstr::Op::CondBr:
                        o << "condbr %" << in.src << ", " << in.a << ", " << in.b; break;
                    case IRInstr::Op::Call: {
                        if (!in.dst.empty()) o << "%" << in.dst << " = ";
                        o << "call " << in.callee << "(";
                        for (size_t i = 0; i < in.args.size(); i++) {
                            if (i) o << ", ";
                            o << "%" << in.args[i];
                        }
                        o << ")";
                        break;
                    }
                    case IRInstr::Op::Print: o << "print %" << in.src; break;
                    case IRInstr::Op::Input:
                        o << "%" << in.dst << " = input";
                        if (!in.src.empty()) o << " %" << in.src;
                        break;
                    case IRInstr::Op::Ret:
                        o << "ret";
                        if (!in.src.empty()) o << " %" << in.src;
                        break;
                }
                o << "\n";
            }
        }
    }
    return o.str();
}

struct Sig {
    std::vector<IRType> params;
    IRType ret = IRType::Void;
};

struct Builder {
    IRModule m;
    IRFunction* fn = nullptr;
    size_t cur = 0; // индекс текущего блока (вектор растёт — указатель нельзя)
    std::map<std::string, IRType> vars;
    std::map<std::string, Sig> sigs; // все функции (проход 1) — рекурсия и любой порядок
    std::string curFile; // исходник текущей функции (для текстов ошибок)
    std::vector<LoopCtx> loops; // стек активных циклов (для break/continue)
    int tmpn = 0;
    int blockn = 0;

    std::string tmp(IRType t) {
        // '$' нельзя написать в исходнике — коллизии с именами исключены
        std::string n = "$t" + std::to_string(tmpn++);
        fn->locals[n] = t;
        return n;
    }
    void emit(IRInstr in) { fn->blocks[cur].instrs.push_back(in); }
    // Новый пустой блок, возвращает его метку. cur НЕ переключает.
    std::string newBlock(const std::string& base) {
        std::string n = base + std::to_string(blockn++);
        IRBlock b; b.label = n;
        fn->blocks.push_back(std::move(b));
        return n;
    }
    size_t idxOf(const std::string& label) {
        for (size_t i = 0; i < fn->blocks.size(); i++)
            if (fn->blocks[i].label == label) return i;
        throw IRBuildError("internal: unknown block " + label);
    }
    // Префикс ошибки с файлом: "path:line N" или "line N".
    std::string at(int line) {
        if (curFile.empty()) return "line " + std::to_string(line);
        return curFile + ":" + std::to_string(line);
    }
    // Br в конец cur-блока, если он ещё не завершён явным return
    // (иначе получился бы мёртвый терминатор после Ret).
    void brTo(const std::string& target) {
        auto& is = fn->blocks[cur].instrs;
        if (!is.empty() && is.back().op == IRInstr::Op::Ret) return;
        IRInstr br; br.op = IRInstr::Op::Br; br.a = target; emit(br);
    }

    // Возвращает (имя значения, тип)
    std::pair<std::string, IRType> gen(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::StrLit: {
                std::string d = tmp(IRType::Str);
                IRInstr in; in.op = IRInstr::Op::ConstStr; in.dst = d;
                in.type = IRType::Str; in.strId = m.addString(e.s);
                emit(in);
                return {d, IRType::Str};
            }
            case Expr::Kind::IntLit: {
                std::string d = tmp(IRType::Int);
                IRInstr in; in.op = IRInstr::Op::ConstInt; in.dst = d;
                in.type = IRType::Int; in.ival = e.ival;
                emit(in);
                return {d, IRType::Int};
            }
            case Expr::Kind::FloatLit: {
                std::string d = tmp(IRType::Float);
                IRInstr in; in.op = IRInstr::Op::ConstFloat; in.dst = d;
                in.type = IRType::Float; in.fval = e.fval;
                emit(in);
                return {d, IRType::Float};
            }
            case Expr::Kind::BoolLit: {
                std::string d = tmp(IRType::Bool);
                IRInstr in; in.op = IRInstr::Op::ConstBool; in.dst = d;
                in.type = IRType::Bool; in.bval = e.bval;
                emit(in);
                return {d, IRType::Bool};
            }
            case Expr::Kind::CharLit: {
                std::string d = tmp(IRType::Char);
                IRInstr in; in.op = IRInstr::Op::ConstChar; in.dst = d;
                in.type = IRType::Char; in.cval = e.cval;
                emit(in);
                return {d, IRType::Char};
            }
            case Expr::Kind::Var: {
                auto it = vars.find(e.s);
                if (it == vars.end())
                    throw IRBuildError(at(e.line) +
                                       ": unknown variable '" + e.s + "'");
                return {e.s, it->second};
            }
            case Expr::Kind::Call: {
                auto it = sigs.find(e.callee);
                if (it == sigs.end())
                    throw IRBuildError(at(e.line) +
                                       ": unknown function '" + e.callee + "'");
                const Sig& sg = it->second;
                if (e.args.size() != sg.params.size())
                    throw IRBuildError(at(e.line) + ": '" + e.callee +
                                       "' expects " + std::to_string(sg.params.size()) +
                                       " argument(s), got " + std::to_string(e.args.size()));
                IRInstr in; in.op = IRInstr::Op::Call; in.callee = e.callee; in.type = sg.ret;
                for (size_t i = 0; i < e.args.size(); i++) {
                    auto [v, t] = gen(*e.args[i]);
                    if (t != sg.params[i])
                        throw IRBuildError(at(e.line) + ": '" + e.callee +
                                           "' argument " + std::to_string(i + 1) + " must be " +
                                           irTypeName(sg.params[i]) + ", got " + irTypeName(t));
                    in.args.push_back(v);
                    in.argTypes.push_back(t);
                }
                if (sg.ret == IRType::Void) {
                    in.dst = "";
                    emit(in);
                    return {"", IRType::Void};
                }
                std::string d = tmp(sg.ret);
                in.dst = d;
                emit(in);
                return {d, sg.ret};
            }
            case Expr::Kind::Input: {
                // input() | input(prompt: str) -> str
                std::string prompt;
                if (e.args.size() == 1) {
                    auto [v, t] = gen(*e.args[0]);
                    if (t != IRType::Str)
                        throw IRBuildError(at(e.line) + ": input() prompt must be str");
                    prompt = v;
                }
                std::string d = tmp(IRType::Str);
                IRInstr in; in.op = IRInstr::Op::Input; in.dst = d; in.src = prompt;
                in.type = IRType::Str;
                emit(in);
                return {d, IRType::Str};
            }
            case Expr::Kind::Neg: {
                auto [v, t] = gen(*e.lhs);
                if (t != IRType::Int && t != IRType::Float)
                    throw IRBuildError(at(e.line) +
                                       ": unary '-' only for int/float");
                std::string d = tmp(t);
                IRInstr in; in.op = IRInstr::Op::Neg; in.dst = d; in.a = v; in.type = t;
                emit(in);
                return {d, t};
            }
            case Expr::Kind::Not: {
                auto [v, t] = gen(*e.lhs);
                if (t != IRType::Bool)
                    throw IRBuildError(at(e.line) +
                                       ": unary '!' only for bool");
                std::string d = tmp(IRType::Bool);
                IRInstr in; in.op = IRInstr::Op::Not; in.dst = d; in.a = v; in.type = IRType::Bool;
                emit(in);
                return {d, IRType::Bool};
            }
            case Expr::Kind::Pow: {
                auto [l, lt] = gen(*e.lhs);
                auto [r, rt] = gen(*e.rhs);
                if (rt != IRType::Int)
                    throw IRBuildError(at(e.line) +
                                       ": exponent of '**' must be int");
                if (lt != IRType::Int && lt != IRType::Float)
                    throw IRBuildError(at(e.line) +
                                       ": base of '**' must be int/float");
                std::string d = tmp(lt);
                IRInstr in; in.op = IRInstr::Op::Pow;
                in.dst = d; in.a = l; in.b = r; in.type = lt;
                emit(in);
                return {d, lt};
            }
            case Expr::Kind::And: case Expr::Kind::Or: {
                auto [l, lt] = gen(*e.lhs);
                auto [r, rt] = gen(*e.rhs);
                if (lt != IRType::Bool || rt != IRType::Bool)
                    throw IRBuildError(at(e.line) +
                                       ": '&&'/'||' only for bool");
                std::string d = tmp(IRType::Bool);
                IRInstr in;
                in.op = (e.kind == Expr::Kind::And) ? IRInstr::Op::And : IRInstr::Op::Or;
                in.dst = d; in.a = l; in.b = r; in.type = IRType::Bool;
                emit(in);
                return {d, IRType::Bool};
            }
            case Expr::Kind::Eq: case Expr::Kind::Ne:
            case Expr::Kind::Lt: case Expr::Kind::Le:
            case Expr::Kind::Gt: case Expr::Kind::Ge: {
                auto [l, lt] = gen(*e.lhs);
                auto [r, rt] = gen(*e.rhs);
                bool isEq = (e.kind == Expr::Kind::Eq || e.kind == Expr::Kind::Ne);
                if (lt != rt)
                    throw IRBuildError(at(e.line) +
                                       ": type mismatch in comparison");
                if (lt == IRType::Str) {
                    if (!isEq)
                        throw IRBuildError(at(e.line) +
                                           ": str supports only '=='/'!='");
                } else if (lt == IRType::Bool || lt == IRType::Char) {
                    if (!isEq)
                        throw IRBuildError(at(e.line) +
                                           ": bool/char support only '=='/'!='");
                } else if (lt != IRType::Int && lt != IRType::Float) {
                    throw IRBuildError(at(e.line) +
                                       ": cannot compare this type");
                }
                std::string d = tmp(IRType::Bool);
                IRInstr in;
                if (e.kind == Expr::Kind::Eq) in.op = IRInstr::Op::Eq;
                if (e.kind == Expr::Kind::Ne) in.op = IRInstr::Op::Ne;
                if (e.kind == Expr::Kind::Lt) in.op = IRInstr::Op::Lt;
                if (e.kind == Expr::Kind::Le) in.op = IRInstr::Op::Le;
                if (e.kind == Expr::Kind::Gt) in.op = IRInstr::Op::Gt;
                if (e.kind == Expr::Kind::Ge) in.op = IRInstr::Op::Ge;
                in.dst = d; in.a = l; in.b = r; in.type = lt; // type = тип операндов
                emit(in);
                return {d, IRType::Bool};
            }
            case Expr::Kind::Add: case Expr::Kind::Sub:
            case Expr::Kind::Mul: case Expr::Kind::Div: case Expr::Kind::Mod: {
                auto [l, lt] = gen(*e.lhs);
                auto [r, rt] = gen(*e.rhs);
                if (e.kind == Expr::Kind::Add && lt == IRType::Str && rt == IRType::Str) {
                    throw IRBuildError(at(e.line) +
                                       ": str + str not implemented in v0.8");
                }
                if (lt != rt)
                    throw IRBuildError(at(e.line) +
                                       ": type mismatch in arithmetic");
                if (e.kind == Expr::Kind::Mod) {
                    if (lt != IRType::Int)
                        throw IRBuildError(at(e.line) + ": '%' is only for int");
                } else if (lt != IRType::Int && lt != IRType::Float) {
                    throw IRBuildError(at(e.line) +
                                       ": arithmetic only for int/float");
                }
                std::string d = tmp(lt);
                IRInstr in;
                if (e.kind == Expr::Kind::Add) in.op = IRInstr::Op::Add;
                if (e.kind == Expr::Kind::Sub) in.op = IRInstr::Op::Sub;
                if (e.kind == Expr::Kind::Mul) in.op = IRInstr::Op::Mul;
                if (e.kind == Expr::Kind::Div) in.op = IRInstr::Op::Div;
                if (e.kind == Expr::Kind::Mod) in.op = IRInstr::Op::Mod;
                in.dst = d; in.a = l; in.b = r; in.type = lt;
                emit(in);
                return {d, lt};
            }
        }
        throw IRBuildError("internal: bad expr");
    }

    void seq(const std::vector<Stmt>& ss) {
        for (auto& s : ss) stmt(s);
    }

    void stmt(const Stmt& s) {
        if (s.kind == Stmt::Kind::Let || s.kind == Stmt::Kind::Const) {
            if (vars.count(s.name))
                throw IRBuildError(at(s.line) + ": redefinition of '" +
                                   s.name + "'");
            auto [v, t] = gen(*s.expr);
            IRType want = t;
            if (s.hasType) {
                want = irTypeFromName(s.type);
                if (want != t)
                    throw IRBuildError(at(s.line) + ": type mismatch: '" +
                                       s.name + "' declared " + irTypeName(want) + ", got " +
                                       irTypeName(t));
            }
            vars[s.name] = want;
            fn->locals[s.name] = want;
            if (s.kind == Stmt::Kind::Const)
                fn->consts.insert(s.name);
            if (v != s.name) {
                IRInstr in; in.op = IRInstr::Op::Copy; in.dst = s.name; in.src = v; in.type = want;
                emit(in);
            }
        } else if (s.kind == Stmt::Kind::Assign) {
            auto it = vars.find(s.name);
            if (it == vars.end())
                throw IRBuildError(at(s.line) + ": unknown variable '" +
                                   s.name + "'");
            if (fn->consts.count(s.name))
                throw IRBuildError(at(s.line) + ": cannot assign to const '" +
                                   s.name + "'");
            auto [v, t] = gen(*s.expr);
            if (t != it->second)
                throw IRBuildError(at(s.line) + ": type mismatch in assignment");
            IRInstr in; in.op = IRInstr::Op::Copy; in.dst = s.name; in.src = v; in.type = t;
            emit(in);
        } else if (s.kind == Stmt::Kind::Print) {
            auto [v, t] = gen(*s.expr);
            if (t != IRType::Str && t != IRType::Int && t != IRType::Float &&
                t != IRType::Bool && t != IRType::Char)
                throw IRBuildError(at(s.line) + ": cannot print this type");
            IRInstr in; in.op = IRInstr::Op::Print; in.src = v; in.type = t;
            emit(in);
        } else if (s.kind == Stmt::Kind::If) {
            ifStmt(s);
        } else if (s.kind == Stmt::Kind::While) {
            whileStmt(s);
        } else if (s.kind == Stmt::Kind::For) {
            forStmt(s);
        } else if (s.kind == Stmt::Kind::Return) {
            IRType want = fn->ret;
            if (want == IRType::Void) {
                if (s.expr)
                    throw IRBuildError(at(s.line) +
                                       ": cannot return a value from void function");
                IRInstr r; r.op = IRInstr::Op::Ret; r.type = IRType::Void;
                emit(r);
            } else {
                if (!s.expr)
                    throw IRBuildError(at(s.line) + ": must return " +
                                       irTypeName(want));
                auto [v, t] = gen(*s.expr);
                if (t != want)
                    throw IRBuildError(at(s.line) + ": return type mismatch" +
                                       ": expected " + irTypeName(want) + ", got " + irTypeName(t));
                IRInstr r; r.op = IRInstr::Op::Ret; r.src = v; r.type = want;
                emit(r);
            }
        } else if (s.kind == Stmt::Kind::Expr) {
            if (!s.expr || s.expr->kind != Expr::Kind::Call)
                throw IRBuildError(at(s.line) +
                                   ": expression statement must be a function call");
            gen(*s.expr); // результат вызова-стейтмента отбрасывается
        } else if (s.kind == Stmt::Kind::Break) {
            if (loops.empty())
                throw IRBuildError(at(s.line) + ": 'break' outside loop");
            brTo(loops.back().breakLabel);
        } else if (s.kind == Stmt::Kind::Continue) {
            if (loops.empty())
                throw IRBuildError(at(s.line) + ": 'continue' outside loop");
            brTo(loops.back().continueLabel);
        }
    }

    void ifStmt(const Stmt& s) {
        auto [v, t] = gen(*s.cond);
        if (t != IRType::Bool)
            throw IRBuildError(at(s.line) + ": if condition must be bool");
        std::string merge = newBlock("merge");
        std::vector<std::string> thenLs;
        thenLs.push_back(newBlock("then"));
        std::vector<std::string> elifCondLs;
        for (size_t i = 0; i < s.elifs.size(); i++) {
            elifCondLs.push_back(newBlock("elif"));
            thenLs.push_back(newBlock("then"));
        }
        std::string elseL = s.elseBranch ? newBlock("else") : merge;
        {
            IRInstr in; in.op = IRInstr::Op::CondBr; in.src = v;
            in.a = thenLs[0];
            in.b = elifCondLs.empty() ? elseL : elifCondLs[0];
            in.type = IRType::Bool;
            emit(in);
        }
        cur = idxOf(thenLs[0]);
        seq(s.thenBranch);
        { brTo(merge); }
        for (size_t i = 0; i < s.elifs.size(); i++) {
            cur = idxOf(elifCondLs[i]);
            auto [ev, et] = gen(*s.elifs[i].cond);
            if (et != IRType::Bool)
                throw IRBuildError(at(s.line) +
                                   ": elif condition must be bool");
            IRInstr in; in.op = IRInstr::Op::CondBr; in.src = ev;
            in.a = thenLs[i + 1];
            in.b = (i + 1 < elifCondLs.size()) ? elifCondLs[i + 1] : elseL;
            in.type = IRType::Bool;
            emit(in);
            cur = idxOf(thenLs[i + 1]);
            seq(s.elifs[i].body);
            brTo(merge);
        }
        if (s.elseBranch) {
            cur = idxOf(elseL);
            seq(*s.elseBranch);
            brTo(merge);
        }
        cur = idxOf(merge);
    }

    void whileStmt(const Stmt& s) {
        std::string condL = newBlock("while");
        std::string bodyL = newBlock("whilebody");
        std::string merge = newBlock("merge");
        loops.push_back({merge, condL});
        { brTo(condL); }
        cur = idxOf(condL);
        auto [v, t] = gen(*s.cond);
        if (t != IRType::Bool)
            throw IRBuildError(at(s.line) + ": while condition must be bool");
        IRInstr in; in.op = IRInstr::Op::CondBr; in.src = v;
        in.a = bodyL; in.b = merge; in.type = IRType::Bool;
        emit(in);
        cur = idxOf(bodyL);
        seq(s.thenBranch);
        { brTo(condL); }
        cur = idxOf(merge);
        loops.pop_back();
    }

    void forStmt(const Stmt& s) {
        // for x: int in lo..hi: ... — границы int, вычисляются один раз.
        auto [lv, lt] = gen(*s.lo);
        auto [rv, rt] = gen(*s.hi);
        if (lt != IRType::Int || rt != IRType::Int)
            throw IRBuildError(at(s.line) + ": for range bounds must be int");
        if (irTypeFromName(s.varType) != IRType::Int)
            throw IRBuildError(at(s.line) + ": for variable must be int");
        if (s.varName == "print" || s.varName == "input")
            throw IRBuildError(at(s.line) + ": '" + s.varName + "' is reserved");
        auto it = vars.find(s.varName);
        if (it == vars.end()) {
            vars[s.varName] = IRType::Int;
            fn->locals[s.varName] = IRType::Int;
        } else {
            if (it->second != IRType::Int)
                throw IRBuildError(at(s.line) + ": for variable '" +
                                   s.varName + "' redeclared with different type");
            if (fn->consts.count(s.varName))
                throw IRBuildError(at(s.line) + ": cannot assign to const '" +
                                   s.varName + "'");
        }
        // x = lo
        { IRInstr in; in.op = IRInstr::Op::Copy; in.dst = s.varName; in.src = lv;
          in.type = IRType::Int; emit(in); }
        std::string condL = newBlock("for");
        std::string bodyL = newBlock("forbody");
        std::string incrL = newBlock("forincr");
        std::string merge = newBlock("merge");
        loops.push_back({merge, incrL});
        { brTo(condL); }
        cur = idxOf(condL);
        std::string cv = tmp(IRType::Bool);
        IRInstr cmp; cmp.op = s.inclusive ? IRInstr::Op::Le : IRInstr::Op::Lt;
        cmp.dst = cv; cmp.a = s.varName; cmp.b = rv; cmp.type = IRType::Int;
        emit(cmp);
        IRInstr in; in.op = IRInstr::Op::CondBr; in.src = cv;
        in.a = bodyL; in.b = merge; in.type = IRType::Bool;
        emit(in);
        cur = idxOf(bodyL);
        seq(s.thenBranch);
        { brTo(incrL); }
        cur = idxOf(incrL);
        // x = x + 1 (continue попадает сюда же — инкремент не пропускается)
        std::string one = tmp(IRType::Int);
        { IRInstr c; c.op = IRInstr::Op::ConstInt; c.dst = one; c.type = IRType::Int;
          c.ival = 1; emit(c); }
        std::string nx = tmp(IRType::Int);
        { IRInstr ad; ad.op = IRInstr::Op::Add; ad.dst = nx; ad.a = s.varName; ad.b = one;
          ad.type = IRType::Int; emit(ad); }
        { IRInstr cp; cp.op = IRInstr::Op::Copy; cp.dst = s.varName; cp.src = nx;
          cp.type = IRType::Int; emit(cp); }
        { brTo(condL); }
        cur = idxOf(merge);
        loops.pop_back();
    }
};

IRModule buildIR(const Program& prog) {
    Builder b;
    // Проход 1: сигнатуры всех функций (любой порядок определений + рекурсия).
    for (auto& f : prog.funcs) {
        b.curFile = f.file;
        if (f.name == "print" || f.name == "input")
            throw IRBuildError(b.at(f.line) + ": '" + f.name + "' is reserved");
        if (b.sigs.count(f.name))
            throw IRBuildError(b.at(f.line) + ": redefinition of function '" +
                               f.name + "' (previous definition also included)");
        Sig sg;
        sg.ret = irTypeFromName(f.ret);
        for (auto& pr : f.params)
            sg.params.push_back(irTypeFromName(pr.type));
        for (size_t i = 0; i < f.params.size(); i++)
            for (size_t j = i + 1; j < f.params.size(); j++)
                if (f.params[i].name == f.params[j].name)
                    throw IRBuildError(b.at(f.line) + ": duplicate parameter '" +
                                       f.params[i].name + "'");
        b.sigs[f.name] = sg;
    }
    auto itMain = b.sigs.find("main");
    if (itMain == b.sigs.end())
        throw IRBuildError("no 'fn main() void' entry point");
    if (!itMain->second.params.empty() || itMain->second.ret != IRType::Void)
        throw IRBuildError("main must be 'fn main() void'");
    // Проход 2: тела функций.
    for (auto& f : prog.funcs) {
        const Sig& sg = b.sigs[f.name];
        b.curFile = f.file;
        b.m.funcs.push_back(IRFunction());
        b.fn = &b.m.funcs.back();
        b.fn->name = f.name;
        b.fn->ret = sg.ret;
        b.vars.clear();
        b.tmpn = 0;
        b.blockn = 0;
        for (size_t i = 0; i < f.params.size(); i++) {
            b.fn->params.push_back({f.params[i].name, sg.params[i]});
            b.vars[f.params[i].name] = sg.params[i];
            b.fn->locals[f.params[i].name] = sg.params[i];
        }
        b.fn->blocks.push_back(IRBlock());
        b.cur = 0;
        for (auto& s : f.body) b.stmt(s);
        // Неявный Ret, если блок не кончается явным return.
        auto& last = b.fn->blocks[b.cur].instrs;
        if (last.empty() || last.back().op != IRInstr::Op::Ret) {
            IRInstr r; r.op = IRInstr::Op::Ret; r.type = sg.ret;
            b.emit(r);
        }
    }
    return b.m;
}
