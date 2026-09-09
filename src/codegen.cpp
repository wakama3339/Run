#include "codegen.h"
#include "x64.h"
#include <cstring>
#include <map>
#include <stdexcept>

// setcc-код для IR-сравнения (isFloat: comisd-флаги, иначе знаковый cmp).
static uint8_t ccFor(IRInstr::Op op, bool isFloat) {
    switch (op) {
        case IRInstr::Op::Eq: return 0x94;
        case IRInstr::Op::Ne: return 0x95;
        case IRInstr::Op::Lt: return isFloat ? 0x92 : 0x9C;
        case IRInstr::Op::Le: return isFloat ? 0x96 : 0x9E;
        case IRInstr::Op::Gt: return isFloat ? 0x97 : 0x9F;
        case IRInstr::Op::Ge: return isFloat ? 0x93 : 0x9D;
        default: throw std::runtime_error("codegen: bad cmp op");
    }
}

// ================= APP (пользовательский код) =================

CoffObject emitAppObj(const IRModule& m) {
    if (m.funcs.empty())
        throw std::runtime_error("codegen: no functions");
    // ABI вызовов runc64 (только для связей пользователь↔пользователь;
    // импорты WinAPI и print_* — по своим соглашениям, см. README):
    //   int/bool/char и половины str — по порядку в RCX,RDX,R8,R9, далее стек;
    //   float — по порядку в XMM0–XMM3, далее стек (общая spill-зона [RSP+32+8k]);
    //   возврат: int — RAX, float — XMM0, str — RAX(ptr)+RDX(len), void — ничего.
    // Кадр: [RSP..RSP+31] — исходящий shadow под вызовы, дальше локалы
    // (включая слоты параметров); входящие стек-аргументы — [RSP+F+40+8k]
    // (кадр + адрес возврата + 32 shadow caller'а).

    X64Buf t;
    RData rd;
    bool needOne = false; // float-**: нужна константа 1.0 в .rdata

    // пул строк модуля -> .rdata
    for (size_t i = 0; i < m.strings.size(); i++)
        rd.addStr("astr_" + std::to_string(i), m.strings[i]);

    // пул double-констант всех функций (уникальные по битам)
    std::map<uint64_t, std::string> dblSym;
    int dblN = 0;
    for (auto& f2 : m.funcs)
        for (auto& b : f2.blocks)
            for (auto& in : b.instrs)
                if (in.op == IRInstr::Op::ConstFloat) {
                    uint64_t bits = 0;
                    memcpy(&bits, &in.fval, 8);
                    if (!dblSym.count(bits)) {
                        std::string nm = "af64_" + std::to_string(dblN++);
                        dblSym[bits] = nm;
                        rd.add(nm, &in.fval, 8);
                    }
                }

    int exitId = -1; // задаётся для main внутри цикла (точка ExitProcess)
    for (auto& fn : m.funcs) {
    bool isMain = (fn.name == "main");
    if (isMain) exitId = t.newLabel();

    // раскладка кадра: [0..31] shadow для вызовов, дальше локалы (str = 16 байт)
    std::map<std::string, uint32_t> slot;
    uint32_t off = 32;
    for (auto& kv : fn.locals) {
        slot[kv.first] = off;
        off += (kv.second == IRType::Str) ? 16 : 8;
    }
    uint32_t frame = off;
    while (frame % 16 != 8) frame += 8;

    auto ltype = [&](const std::string& n) -> IRType {
        auto it = fn.locals.find(n);
        if (it == fn.locals.end()) throw std::runtime_error("codegen: unknown local " + n);
        return it->second;
    };

    t.define(fn.name);
    t.subRsp(frame);

    // Пролог параметров: входящие регистры/стек -> слоты локалов.
    {
        int intUsed = 0, floatUsed = 0, spill = 0;
        for (auto& pr : fn.params) {
            uint32_t s = slot.at(pr.first);
            if (pr.second == IRType::Float) {
                if (floatUsed < 4) {
                    if (floatUsed == 0) t.movsdMemRspXmm0(s);
                    else if (floatUsed == 1) t.movsdMemRspXmm1(s);
                    else if (floatUsed == 2) t.movsdMemRspXmm2(s);
                    else t.movsdMemRspXmm3(s);
                    floatUsed++;
                } else {
                    // входящий стек-аргумент: [RSP+F+40+8k]
                    // (F — кадр, +8 адрес возврата, +32 shadow caller'а)
                    uint32_t off = frame + 40 + (uint32_t)spill * 8; spill++;
                    t.movsdXmm0MemRsp(off);
                    t.movsdMemRspXmm0(s);
                }
            } else {
                int halves = (pr.second == IRType::Str) ? 2 : 1;
                for (int h = 0; h < halves; h++) {
                    uint32_t hs = s + (uint32_t)h * 8;
                    if (intUsed < 4) {
                        if (intUsed == 0) t.movMemRspRcx(hs);
                        else if (intUsed == 1) t.movMemRspRdx(hs);
                        else if (intUsed == 2) t.movMemRspR8(hs);
                        else t.movMemRspR9(hs);
                        intUsed++;
                    } else {
                        uint32_t off = frame + 40 + (uint32_t)spill * 8; spill++;
                        t.movRaxMemRsp(off);
                        t.movMemRspRax(hs);
                    }
                }
            }
        }
    }

    // метки IR-блоков -> локальные метки эмиттера (near-прыжки, без релокаций)
    std::map<std::string, int> blabel;
    for (auto& b : fn.blocks) blabel[b.label] = t.newLabel();
    // Ни один блок не должен проваливаться (fallthrough) в следующий:
    // Br/CondBr прыгают явно, а Ret main прыгает на точку выхода (exitId ниже).

    for (auto& b : fn.blocks) {
        t.bind(blabel.at(b.label));
        for (auto& in : b.instrs) {
            switch (in.op) {
                case IRInstr::Op::ConstStr: {
                    uint32_t d = slot.at(in.dst);
                    std::string sym = "astr_" + std::to_string(in.strId);
                    t.leaRip(0, sym);              // lea rax,[rip+sym]
                    t.movMemRspRax(d);             // [d] = ptr
                    t.movQwordMemRsp(d + 8, (uint32_t)m.strings[in.strId].size());
                    break;
                }
                case IRInstr::Op::ConstInt: {
                    uint32_t d = slot.at(in.dst);
                    t.movRcxImm64((uint64_t)in.ival);
                    t.movMemRspRcx(d);
                    break;
                }
                case IRInstr::Op::ConstFloat: {
                    uint32_t d = slot.at(in.dst);
                    uint64_t bits = 0;
                    memcpy(&bits, &in.fval, 8);
                    t.movsdXmm0Rip(dblSym.at(bits));
                    t.movsdMemRspXmm0(d);
                    break;
                }
                case IRInstr::Op::ConstBool: {
                    uint32_t d = slot.at(in.dst);
                    t.movClImm(in.bval ? 1 : 0);
                    t.movMemRspCl(d);
                    break;
                }
                case IRInstr::Op::ConstChar: {
                    uint32_t d = slot.at(in.dst);
                    t.movClImm((uint8_t)in.cval);
                    t.movMemRspCl(d);
                    break;
                }
                case IRInstr::Op::Copy: {
                    uint32_t d = slot.at(in.dst), s = slot.at(in.src);
                    IRType lt = ltype(in.dst);
                    if (lt == IRType::Float) {
                        t.movsdXmm0MemRsp(s);
                        t.movsdMemRspXmm0(d);
                    } else if (lt == IRType::Str) {
                        t.movRaxMemRsp(s); t.movMemRspRax(d);
                        t.movRaxMemRsp(s + 8); t.movMemRspRax(d + 8);
                    } else if (lt == IRType::Bool || lt == IRType::Char) {
                        t.movzxEcxMemRsp(s);
                        t.movMemRspRcx(d);
                    } else {
                        t.movRaxMemRsp(s);
                        t.movMemRspRax(d);
                    }
                    break;
                }
                case IRInstr::Op::Add:
                case IRInstr::Op::Sub:
                case IRInstr::Op::Mul:
                case IRInstr::Op::Div:
                case IRInstr::Op::Mod: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a), bb = slot.at(in.b);
                    if (in.type == IRType::Float) {
                        t.movsdXmm0MemRsp(a);
                        t.movsdXmm1MemRsp(bb);
                        if (in.op == IRInstr::Op::Add) t.addsdXmm0Xmm1();
                        if (in.op == IRInstr::Op::Sub) t.subsdXmm0Xmm1();
                        if (in.op == IRInstr::Op::Mul) t.mulsdXmm0Xmm1();
                        if (in.op == IRInstr::Op::Div) t.divsdXmm0Xmm1();
                        t.movsdMemRspXmm0(d);
                    } else {
                        t.movRaxMemRsp(a);
                        if (in.op == IRInstr::Op::Add) t.addRaxMemRsp(bb);
                        else if (in.op == IRInstr::Op::Sub) t.subRaxMemRsp(bb);
                        else if (in.op == IRInstr::Op::Mul) t.imulRaxMemRsp(bb);
                        else { t.cqo(); t.idivMemRsp(bb); } // Div и Mod
                        if (in.op == IRInstr::Op::Mod) t.movMemRspRdx(d); // остаток в RDX
                        else t.movMemRspRax(d);
                    }
                    break;
                }
                case IRInstr::Op::Neg: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a);
                    if (in.type == IRType::Float) {
                        t.movsdXmm0MemRsp(a);
                        t.xorpdXmm1();
                        t.subsdXmm1Xmm0();
                        t.movapdXmm0Xmm1();
                        t.movsdMemRspXmm0(d);
                    } else {
                        t.movRaxMemRsp(a);
                        t.negRax();
                        t.movMemRspRax(d);
                    }
                    break;
                }
                case IRInstr::Op::Pow: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a), bb = slot.at(in.b);
                    if (in.type == IRType::Float) {
                        // float ** int: acc-цикл; при exp<0 — 1/acc; exp==0 — 1.0
                        needOne = true;
                        int lZero = t.newLabel(), lPos = t.newLabel();
                        int lLoop = t.newLabel(), lStore = t.newLabel();
                        t.movRcxMemRsp(bb);      // exp
                        t.movEdxImm(0);          // neg = 0
                        t.testRcx();
                        t.jcc(X64Buf::CC_Z, lZero);
                        t.jcc(X64Buf::CC_NS, lPos);
                        t.negRcx();
                        t.movEdxImm(1);
                        t.bind(lPos);
                        t.movsdXmm1MemRsp(a);    // base
                        t.movsdXmm0Rip("af64_one");
                        t.bind(lLoop);
                        t.mulsdXmm0Xmm1();
                        t.decRcx();
                        t.jcc(X64Buf::CC_NZ, lLoop);
                        t.testEdx();
                        t.jcc(X64Buf::CC_Z, lStore);
                        t.movsdXmm1Rip("af64_one");
                        t.divsdXmm1Xmm0();
                        t.movapdXmm0Xmm1();
                        t.jmp(lStore);
                        t.bind(lZero);
                        t.movsdXmm0Rip("af64_one");
                        t.bind(lStore);
                        t.movsdMemRspXmm0(d);
                    } else {
                        // int ** int: exp<0 — 0, иначе цикл умножений
                        int lNeg = t.newLabel(), lLoop = t.newLabel(), lDone = t.newLabel();
                        t.movRaxMemRsp(a);       // base
                        t.movRcxMemRsp(bb);      // exp
                        t.testRcx();
                        t.jcc(X64Buf::CC_S, lNeg);
                        t.movRdx1();             // res = 1
                        t.testRcx();
                        t.jcc(X64Buf::CC_Z, lDone);
                        t.bind(lLoop);
                        t.imulRdxRax();
                        t.decRcx();
                        t.jcc(X64Buf::CC_NZ, lLoop);
                        t.movRaxRdx();
                        t.jmp(lDone);
                        t.bind(lNeg);
                        t.xorEax();
                        t.bind(lDone);
                        t.movMemRspRax(d);
                    }
                    break;
                }
                case IRInstr::Op::Eq:
                case IRInstr::Op::Ne:
                case IRInstr::Op::Lt:
                case IRInstr::Op::Le:
                case IRInstr::Op::Gt:
                case IRInstr::Op::Ge: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a), bb = slot.at(in.b);
                    bool isFloat = (in.type == IRType::Float);
                    uint8_t cc = ccFor(in.op, isFloat);
                    if (in.type == IRType::Str) {
                        // побайтовое сравнение: rsi/ptrA, rdi/ptrB, rcx/lenA, rdx/lenB
                        int lFalse = t.newLabel(), lTrue = t.newLabel();
                        int lLoop = t.newLabel(), lDone = t.newLabel();
                        t.movRsiMemRsp(a);
                        t.movRcxMemRsp(a + 8);
                        t.movRdiMemRsp(bb);
                        t.movRdxMemRsp(bb + 8);
                        t.cmpRdxRcx();           // lenA vs lenB
                        t.jcc(X64Buf::CC_NZ, lFalse);
                        t.testRcx();
                        t.jcc(X64Buf::CC_Z, lTrue); // обе пустые
                        t.xorEdx();              // i = 0
                        t.bind(lLoop);
                        t.movAlSib();
                        t.cmpAlSib();
                        t.jcc(X64Buf::CC_NZ, lFalse);
                        t.incRdx();
                        t.cmpRdxRcx();
                        t.jcc(X64Buf::CC_B, lLoop);
                        t.bind(lTrue);
                        t.movClImm(1);
                        t.jmp(lDone);
                        t.bind(lFalse);
                        t.movClImm(0);
                        t.bind(lDone);
                        if (in.op == IRInstr::Op::Ne) t.xorCl1();
                        t.movMemRspCl(d);
                    } else if (isFloat) {
                        t.movsdXmm0MemRsp(a);
                        t.movsdXmm1MemRsp(bb);
                        t.comisdXmm0Xmm1();
                        t.setccAl(cc);
                        t.movMemRspAl(d);
                    } else if (in.type == IRType::Bool || in.type == IRType::Char) {
                        t.movzxEcxMemRsp(a);
                        t.cmpClMemRsp(bb);
                        t.setccCl(cc);
                        t.movMemRspCl(d);
                    } else {
                        t.movRaxMemRsp(a);
                        t.cmpRaxMemRsp(bb);
                        t.setccAl(cc);
                        t.movMemRspAl(d);
                    }
                    break;
                }
                case IRInstr::Op::And:
                case IRInstr::Op::Or: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a), bb = slot.at(in.b);
                    t.movzxEcxMemRsp(a);
                    if (in.op == IRInstr::Op::And) t.andClMemRsp(bb);
                    else t.orClMemRsp(bb);
                    t.movMemRspCl(d);
                    break;
                }
                case IRInstr::Op::Not: {
                    uint32_t d = slot.at(in.dst), a = slot.at(in.a);
                    t.movzxEcxMemRsp(a);
                    t.xorCl1();
                    t.movMemRspCl(d);
                    break;
                }
                case IRInstr::Op::Br: {
                    t.jmp(blabel.at(in.a));
                    break;
                }
                case IRInstr::Op::CondBr: {
                    uint32_t s = slot.at(in.src);
                    int lt = blabel.at(in.a), lf = blabel.at(in.b);
                    t.movzxEcxMemRsp(s);
                    t.testCl();
                    t.jcc(X64Buf::CC_NZ, lt);
                    t.jmp(lf);
                    break;
                }
                case IRInstr::Op::Print: {
                    uint32_t s = slot.at(in.src);
                    if (in.type == IRType::Str) {
                        t.movRcxMemRsp(s);
                        t.movRdxMemRsp(s + 8);
                        t.callSym("print_str");
                    } else if (in.type == IRType::Int) {
                        t.movRcxMemRsp(s);
                        t.callSym("print_int");
                    } else if (in.type == IRType::Float) {
                        t.movsdXmm0MemRsp(s);
                        t.callSym("print_float");
                    } else if (in.type == IRType::Bool) {
                        t.movzxEcxMemRsp(s);
                        t.callSym("print_bool");
                    } else if (in.type == IRType::Char) {
                        t.movzxEcxMemRsp(s);
                        t.callSym("print_char");
                    }
                    break;
                }
                case IRInstr::Op::Input: {
                    uint32_t d = slot.at(in.dst);
                    if (!in.src.empty()) {
                        // промпт без перевода строки (напрямую в __write_stdout)
                        uint32_t s = slot.at(in.src);
                        t.movRcxMemRsp(s);
                        t.movRdxMemRsp(s + 8);
                        t.callSym("__write_stdout");
                    }
                    t.callSym("__read_line"); // RAX=ptr, RDX=len (куча)
                    t.movMemRspRax(d);
                    t.movMemRspRdx(d + 8);
                    break;
                }
                case IRInstr::Op::Call: {
                    // Вызов по ABI runc64: сначала раскладка, потом moves.
                    int intUsed = 0, floatUsed = 0, spill = 0;
                    struct Move { bool isFloat; int reg; int spillIdx; uint32_t slot; };
                    std::vector<Move> moves;
                    for (size_t k = 0; k < in.args.size(); k++) {
                        uint32_t s = slot.at(in.args[k]);
                        IRType at = in.argTypes[k];
                        if (at == IRType::Float) {
                            if (floatUsed < 4) moves.push_back({true, floatUsed++, -1, s});
                            else moves.push_back({true, -1, spill++, s});
                        } else if (at == IRType::Str) {
                            if (intUsed < 4) moves.push_back({false, intUsed++, -1, s});
                            else moves.push_back({false, -1, spill++, s});
                            if (intUsed < 4) moves.push_back({false, intUsed++, -1, s + 8});
                            else moves.push_back({false, -1, spill++, s + 8});
                        } else {
                            if (intUsed < 4) moves.push_back({false, intUsed++, -1, s});
                            else moves.push_back({false, -1, spill++, s});
                        }
                    }
                    uint32_t alloc = 40 + (uint32_t)spill * 8;
                    if (spill % 2 == 1) alloc += 8; // выравнивание под call
                    t.subRsp(alloc);
                    // ВАЖНО: RSP сдвинут на alloc — слоты читаем как slot+alloc,
                    // spill-зона [RSP+32+8k] уже пост-сабовая.
                    for (auto& mv : moves) {
                        uint32_t base = mv.slot + alloc;
                        if (mv.isFloat) {
                            if (mv.reg == 0) t.movsdXmm0MemRsp(base);
                            else if (mv.reg == 1) t.movsdXmm1MemRsp(base);
                            else if (mv.reg == 2) t.movsdXmm2MemRsp(base);
                            else if (mv.reg == 3) t.movsdXmm3MemRsp(base);
                            else {
                                t.movsdXmm0MemRsp(base);
                                t.movsdMemRspXmm0(32 + (uint32_t)mv.spillIdx * 8);
                            }
                        } else {
                            if (mv.reg == 0) t.movRcxMemRsp(base);
                            else if (mv.reg == 1) t.movRdxMemRsp(base);
                            else if (mv.reg == 2) t.movR8MemRsp(base);
                            else if (mv.reg == 3) t.movR9MemRsp(base);
                            else {
                                t.movRaxMemRsp(base);
                                t.movMemRspRax(32 + (uint32_t)mv.spillIdx * 8);
                            }
                        }
                    }
                    t.callSym(in.callee);
                    t.addRsp(alloc);
                    if (in.type == IRType::Float) {
                        t.movsdMemRspXmm0(slot.at(in.dst));
                    } else if (in.type == IRType::Str) {
                        t.movMemRspRax(slot.at(in.dst));
                        t.movMemRspRdx(slot.at(in.dst) + 8);
                    } else if (in.type != IRType::Void) {
                        t.movMemRspRax(slot.at(in.dst));
                    }
                    break;
                }
                case IRInstr::Op::Ret: {
                    if (in.type == IRType::Float) {
                        t.movsdXmm0MemRsp(slot.at(in.src));
                    } else if (in.type == IRType::Str) {
                        t.movRaxMemRsp(slot.at(in.src));
                        t.movRdxMemRsp(slot.at(in.src) + 8);
                    } else if (in.type != IRType::Void) {
                        if (in.type == IRType::Bool || in.type == IRType::Char)
                            t.movzxEaxMemRsp(slot.at(in.src));
                        else
                            t.movRaxMemRsp(slot.at(in.src));
                    }
                    if (isMain) {
                        t.jmp(exitId); // main: завершение процесса
                    } else {
                        t.addRsp(frame);
                        t.ret();
                    }
                    break;
                }
            }
        }
    }

    } // for (auto& fn : m.funcs)

    // main — точка входа EXE: завершаемся через ExitProcess(0).
    // Сюда попадают только прыжком из Ret (проваливания снизу нет,
    // т.к. каждый блок кончается Br/CondBr/Ret).
    if (exitId >= 0) {
        t.bind(exitId);
        t.xorEcx();
        t.callImp("__imp_ExitProcess");
    }

    if (needOne && !rd.syms.count("af64_one")) {
        double one = 1.0;
        rd.add("af64_one", &one, 8);
    }

    CoffObject o;
    o.text = t.code;
    for (auto& f : t.fixups) {
        CoffReloc r; r.offset = f.offset; r.symName = f.symbol; r.type = RELOC_REL32;
        o.textRelocs.push_back(r);
    }
    o.textSyms = t.labels;
    o.rdata = rd.bytes;
    o.rdataSyms = rd.syms;
    o.undefined = {"print_str", "print_int", "print_float", "print_bool",
                   "print_char", "__write_stdout", "__read_line", "__imp_ExitProcess"};
    return o;
}

// ================= BUILTINS (stdlib) =================
// Кадр __write_stdout = 56: [0..31] shadow, [32..39] Overlapped=NULL,
// [40..47] len, [44..47] written(dword), [48..55] buf.

static void emitWriteStdout(X64Buf& t) {
    t.define("__write_stdout");
    t.subRsp(56);
    t.movMemRspRcx(48);          // buf
    t.movMemRspRdx(40);          // len
    t.movEcxImm(-11);            // STD_OUTPUT_HANDLE
    t.callImp("__imp_GetStdHandle");
    t.movRcxRax();               // hFile
    t.movRdxMemRsp(48);          // buffer
    t.movR8dMemRsp(40);          // nbytes
    t.leaR9Rsp(44);              // &written
    t.movDwordMemRsp(44, 0);
    t.movQwordMemRsp(32, 0);     // Overlapped = NULL
    t.callImp("__imp_WriteFile");
    t.addRsp(56);
    t.ret();
}

static void emitPrintStr(X64Buf& t) {
    t.define("print_str");
    t.subRsp(40);
    t.callSym("__write_stdout"); // (rcx,rdx уже на месте)
    t.leaRcx("__b_nl");
    t.movEdxImm(2);
    t.callSym("__write_stdout");
    t.addRsp(40);
    t.ret();
}

static void emitPrintChar(X64Buf& t) {
    t.define("print_char");
    t.subRsp(56);
    t.movMemRspCl(40);
    t.leaRcxRsp(40);
    t.movEdxImm(1);
    t.callSym("__write_stdout");
    t.leaRcx("__b_nl");
    t.movEdxImm(2);
    t.callSym("__write_stdout");
    t.addRsp(56);
    t.ret();
}

static void emitPrintBool(X64Buf& t) {
    t.define("print_bool");
    int lFalse = t.newLabel(), lNl = t.newLabel();
    t.subRsp(40);
    t.testCl();
    t.jcc(X64Buf::CC_Z, lFalse);
    t.leaRcx("__b_true");
    t.movEdxImm(4);
    t.callSym("__write_stdout");
    t.jmp(lNl);
    t.bind(lFalse);
    t.leaRcx("__b_false");
    t.movEdxImm(5);
    t.callSym("__write_stdout");
    t.bind(lNl);
    t.leaRcx("__b_nl");
    t.movEdxImm(2);
    t.callSym("__write_stdout");
    t.addRsp(40);
    t.ret();
}

static void emitPrintInt(X64Buf& t) {
    // кадр 72: [0..31] shadow, буфер [32..63], конец = rsp+64
    t.define("print_int");
    int lPos = t.newLabel(), lDig = t.newLabel(), lNoSign = t.newLabel();
    t.subRsp(72);
    t.movRaxRcx();
    t.leaRsiRsp(64);             // rsi = end
    t.u8(0x31); t.u8(0xFF);      // xor edi,edi (neg=0)
    t.testRax();
    t.jcc(X64Buf::CC_NS, lPos);  // jns Lpos
    t.negRax();
    t.u8(0x48); t.u8(0xC7); t.u8(0xC7); t.u32(1); // mov rdi,1
    t.bind(lPos);
    t.movRcx10();
    t.bind(lDig);
    t.xorEdx();
    t.divRcx();
    t.addDl30();
    t.decRsi();
    t.movMemRsiDl();
    t.testRax();
    t.jcc(X64Buf::CC_NZ, lDig);
    t.u8(0x48); t.u8(0x85); t.u8(0xFF); // test rdi,rdi
    t.jcc(X64Buf::CC_Z, lNoSign);
    t.decRsi();
    t.movByteMemRsi('-');
    t.bind(lNoSign);
    t.leaRdxRsp(64);             // rdx = end
    t.u8(0x48); t.u8(0x29); t.u8(0xF2); // sub rdx,rsi (len)
    t.u8(0x48); t.u8(0x89); t.u8(0xF1); // mov rcx,rsi (ptr)
    t.callSym("__write_stdout");
    t.leaRcx("__b_nl");
    t.movEdxImm(2);
    t.callSym("__write_stdout");
    t.addRsp(72);
    t.ret();
}

static void emitPrintFloat(X64Buf& t) {
    t.define("print_float");
    int lPos = t.newLabel(), lDigits = t.newLabel(), lIntDone = t.newLabel();
    int lNoSign = t.newLabel(), lFrac = t.newLabel(), lFracDone = t.newLabel();
    t.subRsp(120);
    t.movsdMemRspXmm0(96);       // [96] = x
    t.u8(0x31); t.u8(0xFF);      // xor edi,edi (neg=0)
    t.xorpdXmm1();
    t.comisdXmm0Xmm1();          // x ? 0
    t.jcc(X64Buf::CC_AE, lPos);  // jae Lpos
    t.subsdXmm1Xmm0();           // xmm1 = -x
    t.movapdXmm0Xmm1();
    t.u8(0x48); t.u8(0xC7); t.u8(0xC7); t.u32(1); // mov rdi,1
    t.bind(lPos);
    t.movsdMemRspXmm0(96);       // [96] = |x|
    t.cvttsd2siRaxXmm0();        // rax = intpart
    t.movMemRspRax(112);         // [112] = intpart
    t.leaRsiRsp(64);             // rsi = mid
    t.movRcx10();
    t.testRax();
    t.jcc(X64Buf::CC_NZ, lDigits);
    t.decRsi();                  // intpart == 0 -> '0'
    t.movByteMemRsi('0');
    t.jmp(lIntDone);
    t.bind(lDigits);
    t.xorEdx();
    t.divRcx();
    t.addDl30();
    t.decRsi();
    t.movMemRsiDl();
    t.testRax();
    t.jcc(X64Buf::CC_NZ, lDigits);
    t.bind(lIntDone);
    t.u8(0x48); t.u8(0x85); t.u8(0xFF); // test rdi,rdi
    t.jcc(X64Buf::CC_Z, lNoSign);
    t.decRsi();
    t.movByteMemRsi('-');
    t.bind(lNoSign);
    t.movMemRspRsi(104);         // [104] = start
    // '.' по адресу mid (rsp+64): C6 84 24 40 00 00 00 2E
    t.u8(0xC6); t.u8(0x84); t.u8(0x24); t.u32(64); t.u8(0x2E);
    // frac = (|x| - (double)intpart) * 1e6
    t.movsdXmm0MemRsp(96);
    t.movRaxMemRsp(112);
    t.cvtsi2sdXmm1Rax();
    t.subsdXmm0Xmm1();
    t.mulsdXmm0Rip("__b_1e6");
    t.cvttsd2siRaxXmm0();        // rax = frac (0..999999)
    t.leaRsiRsp(71);             // rsi = mid+7
    t.movRcx10();
    // счётчик 6 в r8: 49 C7 C0 06 00 00 00
    t.u8(0x49); t.u8(0xC7); t.u8(0xC0); t.u32(6);
    t.bind(lFrac);
    t.xorEdx();
    t.divRcx();
    t.addDl30();
    t.decRsi();
    t.movMemRsiDl();
    t.u8(0x49); t.u8(0xFF); t.u8(0xC8); // dec r8
    t.jcc(X64Buf::CC_NZ, lFrac);
    t.bind(lFracDone);
    // write(start, 71-start)
    t.movRsiMemRsp(104);         // rsi = start
    t.leaRdxRsp(71);             // rdx = end
    t.u8(0x48); t.u8(0x29); t.u8(0xF2); // sub rdx,rsi
    t.u8(0x48); t.u8(0x89); t.u8(0xF1); // mov rcx,rsi
    t.callSym("__write_stdout");
    t.leaRcx("__b_nl");
    t.movEdxImm(2);
    t.callSym("__write_stdout");
    t.addRsp(120);
    t.ret();
}

// __read_line(): читает строку stdin до '\n'/EOF, возвращает RAX=ptr (куча),
// RDX=len (без '\r\n'). Куча — bump-аллокатор в .data (__heap_ptr/__heap_base).
// Кадр 584: [0..31] shadow, [32..39] Overlapped=NULL, [40..47] written/dst-save,
// [48..55] handle, [56..63] total, [64..71] n, buf [72..583] (512 байт).
static void emitReadLine(X64Buf& t) {
    t.define("__read_line");
    int lLoop = t.newLabel(), lScan = t.newLabel(), lFound = t.newLabel();
    int lAlloc = t.newLabel(), lHaveHeap = t.newLabel(), lOom = t.newLabel();
    int lDone = t.newLabel(), lRet = t.newLabel();
    t.subRsp(584);
    t.movEcxImm(-10); // STD_INPUT_HANDLE
    t.callImp("__imp_GetStdHandle");
    t.movMemRspRax(48);      // handle
    t.movQwordMemRsp(56, 0); // total = 0
    t.bind(lLoop);
    t.movRaxMemRsp(56);
    t.cmpRaxImm(512);
    t.jcc(X64Buf::CC_AE, lDone); // буфер полон — вернуть как есть
    t.movRcxMemRsp(48);          // handle
    t.leaRdxRsp(72);
    t.addRdxMemRsp(56);          // buf + total
    t.movEaxImm(512);
    t.subEaxMemRsp(56);
    t.movR8dEax();               // remaining
    t.leaR9Rsp(64);
    t.movDwordMemRsp(64, 0);     // &n
    t.movQwordMemRsp(32, 0);     // Overlapped = NULL
    t.callImp("__imp_ReadFile");
    t.testEax();
    t.jcc(X64Buf::CC_Z, lDone);  // ошибка чтения
    t.movEdxMemRsp(64);
    t.testEdx();
    t.jcc(X64Buf::CC_Z, lDone);  // EOF
    t.movEaxMemRsp(64);
    t.movRcxMemRsp(56);
    t.u8(0x48); t.u8(0x01); t.u8(0xC1); // add rcx,rax (total += n)
    t.movMemRspRcx(56);
    t.leaRsiRsp(72);             // rsi = buf
    t.movRcxMemRsp(56);          // rcx = total
    t.bind(lScan);
    t.testRcx();
    t.jcc(X64Buf::CC_Z, lLoop);  // '\n' нет — читаем дальше
    t.cmpByteMemRsi(0x0A);
    t.jcc(X64Buf::CC_Z, lFound);
    t.incRsi();
    t.decRcx();
    t.jmp(lScan);
    t.bind(lFound);
    // rsi -> '\n', len = rsi - buf; срезать trailing '\r'
    t.movRdxRsi();
    t.leaRcxRsp(72);
    t.subRdxRcx();
    t.testRdx();
    t.jcc(X64Buf::CC_Z, lAlloc);
    t.decRsi();
    t.cmpByteMemRsi(0x0D);
    t.jcc(X64Buf::CC_NZ, lAlloc);
    t.decRdx();
    t.bind(lAlloc);
    // rdx = точная длина; heap-alloc с выравниванием 8
    t.movR8Rdx();                // r8 = len (save)
    t.addRdxImm(7);
    t.andRdxImm(0xFFFFFFF8u);
    t.movRaxRip("__heap_ptr");
    t.testRax();
    t.jcc(X64Buf::CC_NZ, lHaveHeap);
    t.leaRip(0, "__heap_base");
    t.movRipRax("__heap_ptr");
    t.bind(lHaveHeap);
    t.movRdiRax();               // dst
    t.addRaxRdx();               // new heap ptr
    t.leaRcx("__heap_base");
    t.addRcxImm(65536);
    t.cmpRaxRcx();
    t.jcc(X64Buf::CC_A, lOom);   // куча переполнена
    t.movRipRax("__heap_ptr");
    t.leaRsiRsp(72);             // src = buf
    t.movRcxR8();                // count = len
    t.movMemRspRdi(40);          // save dst (rep movsb двигает rsi/rdi!)
    t.repMovsb();
    t.movRaxMemRsp(40);          // ptr = dst
    t.movRdxR8();                // len
    t.jmp(lRet);
    t.bind(lDone);
    // усечение/EOF/ошибка: len = total, конец = buf + total
    t.movRdxMemRsp(56);
    t.leaRsiRsp(72);
    t.addRsiRdx();
    t.testRdx();
    t.jcc(X64Buf::CC_Z, lAlloc);
    t.decRsi();
    t.cmpByteMemRsi(0x0D);
    t.jcc(X64Buf::CC_NZ, lAlloc);
    t.decRdx();
    t.jmp(lAlloc);
    t.bind(lOom);
    t.movEcxImm(1);
    t.callImp("__imp_ExitProcess");
    t.bind(lRet);
    t.addRsp(584);
    t.ret();
}

CoffObject emitBuiltinsObj() {
    X64Buf t;
    RData rd;
    const char crlf[] = "\r\n";
    rd.add("__b_nl", crlf, 2);
    rd.add("__b_true", "true", 4);
    rd.add("__b_false", "false", 5);
    double e6 = 1000000.0;
    rd.add("__b_1e6", &e6, 8);

    emitWriteStdout(t);
    emitPrintStr(t);
    emitPrintChar(t);
    emitPrintBool(t);
    emitPrintInt(t);
    emitPrintFloat(t);
    emitReadLine(t);

    CoffObject o;
    o.text = t.code;
    for (auto& f : t.fixups) {
        CoffReloc r; r.offset = f.offset; r.symName = f.symbol; r.type = RELOC_REL32;
        o.textRelocs.push_back(r);
    }
    o.textSyms = t.labels;
    o.rdata = rd.bytes;
    o.rdataSyms = rd.syms;
    // .data: __heap_ptr (8 байт, 0 = не инициализирована) + __heap_base (BSS 64K)
    {
        uint64_t zero = 0;
        o.data.assign((uint8_t*)&zero, (uint8_t*)&zero + 8);
        o.dataSyms["__heap_ptr"] = 0;
        o.dataSyms["__heap_base"] = 8;
        o.dataVirtualExtra = 65536;
    }
    o.undefined = {"__imp_GetStdHandle", "__imp_WriteFile", "__imp_ReadFile",
                   "__imp_ExitProcess"};
    return o;
}
