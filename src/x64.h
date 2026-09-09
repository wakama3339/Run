#pragma once
// X64: ручной эмиттер машинного кода AMD64 (Windows x64 ABI).
// Никакого LLVM/ассемблера: байты инструкций конструируются здесь.
// Все ссылки на данные/функции/импорты — через REL32-фиксапы,
// которые COFF writer превращает в релокации, а linker чинит.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

const uint8_t RELOC_REL32 = 4; // IMAGE_REL_AMD64_REL32

struct Fixup {
    uint32_t offset = 0;   // смещение в .text, где лежит rel32
    std::string symbol;    // цель
};

class X64Buf {
public:
    std::vector<uint8_t> code;
    std::vector<Fixup> fixups;
    std::map<std::string, uint32_t> labels; // определённые функции -> offset

    uint32_t pos() const { return (uint32_t)code.size(); }
    void define(const std::string& name) { labels[name] = pos(); }

    void u8(uint8_t v) { code.push_back(v); }
    void u32(uint32_t v) {
        code.push_back(v & 0xFF); code.push_back((v >> 8) & 0xFF);
        code.push_back((v >> 16) & 0xFF); code.push_back((v >> 24) & 0xFF);
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; i++) code.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) code.push_back(b[i]);
    }
    void disp32placeholder(const std::string& sym) {
        Fixup f; f.offset = pos(); f.symbol = sym;
        fixups.push_back(f);
        u32(0);
    }

    // ---- локальные метки для прыжков внутри функции (near rel32, без релокаций)
    int newLabel() { return nextLabel_++; }
    void bind(int id) {
        uint32_t here = pos();
        auto it = patches_.find(id);
        if (it != patches_.end()) {
            for (uint32_t at : it->second) {
                int32_t d = (int32_t)(here - (at + 4));
                code[at] = d & 0xFF; code[at + 1] = (d >> 8) & 0xFF;
                code[at + 2] = (d >> 16) & 0xFF; code[at + 3] = (d >> 24) & 0xFF;
            }
            patches_.erase(it);
        }
        bound_[id] = here;
    }
    void patchTo(int id, uint32_t dispAt) {
        auto b = bound_.find(id);
        if (b != bound_.end()) {
            int32_t d = (int32_t)(b->second - (dispAt + 4));
            code[dispAt] = d & 0xFF; code[dispAt + 1] = (d >> 8) & 0xFF;
            code[dispAt + 2] = (d >> 16) & 0xFF; code[dispAt + 3] = (d >> 24) & 0xFF;
        } else {
            patches_[id].push_back(dispAt);
        }
    }
    enum Cond : uint8_t { CC_Z = 0x84, CC_NZ = 0x85, CC_B = 0x82, CC_AE = 0x83,
                          CC_S = 0x88, CC_NS = 0x89, CC_A = 0x87, CC_BE = 0x96 };
    void jcc(Cond cc, int label) {
        u8(0x0F); u8((uint8_t)cc);
        uint32_t at = pos(); u32(0);
        patchTo(label, at);
    }
    void jmp(int label) {
        u8(0xE9);
        uint32_t at = pos(); u32(0);
        patchTo(label, at);
    }

    // ---- вызовы / адреса (через COFF-релокации)
    void callSym(const std::string& sym) { u8(0xE8); disp32placeholder(sym); } // E8 rel32
    void callImp(const std::string& impSym) { u8(0xFF); u8(0x15); disp32placeholder(impSym); } // FF15 [rip+rel]

    // lea r64, [rip+sym]: rexW 8D modrm(=reg<<3|101) rel32
    void leaRip(uint8_t reg, const std::string& sym) {
        u8(0x48); u8(0x8D); u8((uint8_t)((reg << 3) | 0x05));
        disp32placeholder(sym);
    }
    void leaRcx(const std::string& sym) { leaRip(1, sym); }
    // movsd xmm0, [rip+sym]: F2 0F 10 05 rel32
    void movsdXmm0Rip(const std::string& sym) {
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x05);
        disp32placeholder(sym);
    }
    // mulsd xmm0, [rip+sym]: F2 0F 59 05 rel32
    void mulsdXmm0Rip(const std::string& sym) {
        u8(0xF2); u8(0x0F); u8(0x59); u8(0x05);
        disp32placeholder(sym);
    }

    // ---- пролог/эпилог кадра
    void subRsp(uint32_t n) {
        if (n <= 127) { u8(0x48); u8(0x83); u8(0xEC); u8((uint8_t)n); }
        else { u8(0x48); u8(0x81); u8(0xEC); u32(n); }
    }
    void addRsp(uint32_t n) {
        if (n <= 127) { u8(0x48); u8(0x83); u8(0xC4); u8((uint8_t)n); }
        else { u8(0x48); u8(0x81); u8(0xC4); u32(n); }
    }
    void ret() { u8(0xC3); }

    // ---- часто используемые инструкции
    void movEcxImm(int32_t v) { u8(0xB9); u32((uint32_t)v); }        // B9 id (ecx = -11)
    void movEdxImm(uint32_t v) { u8(0xBA); u32(v); }                  // BA id
    void movRcxImm64(uint64_t v) { u8(0x48); u8(0xB9); u64(v); }      // 48 B9 iq
    void movClImm(uint8_t v) { u8(0xB1); u8(v); }                     // B1 ib
    void movRcxRax() { u8(0x48); u8(0x89); u8(0xC1); }                // mov rcx,rax
    void movRdxRax() { u8(0x48); u8(0x89); u8(0xC2); }                // mov rdx,rax
    void movRaxRcx() { u8(0x48); u8(0x89); u8(0xC8); }                // mov rax,rcx
    void movRdxRsi() { u8(0x48); u8(0x89); u8(0xF2); }                // mov rdx,rsi
    void xorEcx() { u8(0x31); u8(0xC9); }                             // xor ecx,ecx
    void xorEdx() { u8(0x31); u8(0xD2); }                             // xor edx,edx
    void testRax() { u8(0x48); u8(0x85); u8(0xC0); }                  // test rax,rax
    void testCl() { u8(0x84); u8(0xC9); }                             // test cl,cl
    void negRax() { u8(0x48); u8(0xF7); u8(0xD8); }                   // neg rax
    void cqo() { u8(0x48); u8(0x99); }                                // cqo
    void movRcx10() { u8(0x48); u8(0xC7); u8(0xC1); u32(10); }        // mov rcx,10
    void divRcx() { u8(0x48); u8(0xF7); u8(0xF1); }                   // div rcx
    void idivMemRsp(uint32_t d) { u8(0x48); u8(0xF7); u8(0xBC); u8(0x24); u32(d); } // idiv [rsp+d]
    void addDl30() { u8(0x80); u8(0xC2); u8(0x30); }                  // add dl,'0'
    void movMemRsiDl() { u8(0x88); u8(0x16); }                        // mov [rsi],dl
    void movByteMemRsi(char v) { u8(0xC6); u8(0x06); u8((uint8_t)v); }// mov byte [rsi],imm8
    void decRsi() { u8(0x48); u8(0xFF); u8(0xCE); }                   // dec rsi
    // mov r64, [rsp+d]: rexW 8B modrm SIB disp32 ; reg: rax=0 rcx=1 rdx=2 rsi=6 rdi=7 r8->REX.R
    void movRegMemRsp(uint8_t reg, uint32_t d, bool rexR = false) {
        u8(rexR ? 0x4C : 0x48); u8(0x8B);
        u8((uint8_t)(0x84 | (reg << 3))); u8(0x24); u32(d);
    }
    void movRaxMemRsp(uint32_t d) { movRegMemRsp(0, d); }
    void movRcxMemRsp(uint32_t d) { movRegMemRsp(1, d); }
    void movRdxMemRsp(uint32_t d) { movRegMemRsp(2, d); }
    void movRsiMemRsp(uint32_t d) { movRegMemRsp(6, d); }
    void movRdiMemRsp(uint32_t d) { movRegMemRsp(7, d); }
    void movR8MemRsp(uint32_t d) { // mov r8,[rsp+d]: 4C 8B 84 24 d32
        u8(0x4C); u8(0x8B); u8(0x84); u8(0x24); u32(d);
    }
    void movR9MemRsp(uint32_t d) { // mov r9,[rsp+d]: 4C 8B 8C 24 d32
        u8(0x4C); u8(0x8B); u8(0x8C); u8(0x24); u32(d);
    }
    void movR8dMemRsp(uint32_t d) { // mov r8d,[rsp+d]: 44 8B 84 24 d32
        u8(0x44); u8(0x8B); u8(0x84); u8(0x24); u32(d);
    }
    void movEdxMemRsp(uint32_t d) { // mov edx,[rsp+d]: 8B 94 24 d32
        u8(0x8B); u8(0x94); u8(0x24); u32(d);
    }
    void movzxEcxMemRsp(uint32_t d) { // movzx ecx,byte [rsp+d]: 0F B6 8C 24 d32
        u8(0x0F); u8(0xB6); u8(0x8C); u8(0x24); u32(d);
    }
    // mov [rsp+d], r64
    void movMemRspReg(uint32_t d, uint8_t reg, bool rexR = false) {
        u8(rexR ? 0x4C : 0x48); u8(0x89);
        u8((uint8_t)(0x84 | (reg << 3))); u8(0x24); u32(d);
    }
    void movMemRspRax(uint32_t d) { movMemRspReg(d, 0); }
    void movMemRspRcx(uint32_t d) { movMemRspReg(d, 1); }
    void movMemRspRdx(uint32_t d) { movMemRspReg(d, 2); }
    void movMemRspRsi(uint32_t d) { movMemRspReg(d, 6); }
    void movMemRspRdi(uint32_t d) { movMemRspReg(d, 7); }
    void movMemRspR8(uint32_t d) { // mov [rsp+d],r8: 4C 89 84 24 d32
        u8(0x4C); u8(0x89); u8(0x84); u8(0x24); u32(d);
    }
    void movMemRspR9(uint32_t d) { // mov [rsp+d],r9: 4C 89 8C 24 d32
        u8(0x4C); u8(0x89); u8(0x8C); u8(0x24); u32(d);
    }
    void movMemRspCl(uint32_t d) { // mov [rsp+d],cl: 88 8C 24 d32
        u8(0x88); u8(0x8C); u8(0x24); u32(d);
    }
    void movMemRspR8d(uint32_t d) {} // placeholder (не используется)
    // lea r64, [rsp+d]
    void leaRegRsp(uint8_t reg, uint32_t d, bool rexR = false) {
        u8(rexR ? 0x4C : 0x48); u8(0x8D);
        u8((uint8_t)(0x84 | (reg << 3))); u8(0x24); u32(d);
    }
    void leaRcxRsp(uint32_t d) { leaRegRsp(1, d); }
    void leaRsiRsp(uint32_t d) { leaRegRsp(6, d); }
    void leaR9Rsp(uint32_t d) { leaRegRsp(1, d, true); } // r9 = reg(1)+REX.R
    void leaRdxRsp(uint32_t d) { leaRegRsp(2, d); }
    void movDwordMemRsp(uint32_t d, uint32_t v) { // C7 84 24 d32 v32
        u8(0xC7); u8(0x84); u8(0x24); u32(d); u32(v);
    }
    void movQwordMemRsp(uint32_t d, uint32_t v) { // 48 C7 84 24 d32 v32
        u8(0x48); u8(0xC7); u8(0x84); u8(0x24); u32(d); u32(v);
    }
    // SSE для float:
    void cvttsd2siRaxXmm0() { u8(0xF2); u8(0x48); u8(0x0F); u8(0x2C); u8(0xC0); }
    void cvtsi2sdXmm1Rax() { u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A); u8(0xC8); }
    void subsdXmm0Xmm1() { u8(0xF2); u8(0x0F); u8(0x5C); u8(0xC1); }
    void addsdXmm0Xmm1() { u8(0xF2); u8(0x0F); u8(0x58); u8(0xC1); }
    void mulsdXmm0Xmm1() { u8(0xF2); u8(0x0F); u8(0x59); u8(0xC1); }
    void divsdXmm0Xmm1() { u8(0xF2); u8(0x0F); u8(0x5E); u8(0xC1); }
    void movapdXmm1Xmm0() { u8(0x66); u8(0x0F); u8(0x28); u8(0xC8); }
    void xorpdXmm1() { u8(0x66); u8(0x0F); u8(0x57); u8(0xC9); } // xorpd xmm1,xmm1
    void comisdXmm0Xmm1() { u8(0x66); u8(0x0F); u8(0x2F); u8(0xC1); }
    void subsdXmm1Xmm0() { u8(0xF2); u8(0x0F); u8(0x5C); u8(0xC8); } // subsd xmm1,xmm0
    void movapdXmm0Xmm1() { u8(0x66); u8(0x0F); u8(0x28); u8(0xC1); }
    void movsdXmm0MemRsp(uint32_t d) { // F2 0F 10 84 24 d32
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x84); u8(0x24); u32(d);
    }
    void movsdXmm1MemRsp(uint32_t d) { // F2 0F 10 8C 24 d32
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x8C); u8(0x24); u32(d);
    }
    void movsdXmm2MemRsp(uint32_t d) { // F2 0F 10 94 24 d32
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x94); u8(0x24); u32(d);
    }
    void movsdXmm3MemRsp(uint32_t d) { // F2 0F 10 9C 24 d32
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x9C); u8(0x24); u32(d);
    }
    void movsdMemRspXmm0(uint32_t d) { // F2 0F 11 84 24 d32
        u8(0xF2); u8(0x0F); u8(0x11); u8(0x84); u8(0x24); u32(d);
    }
    void movsdMemRspXmm1(uint32_t d) { // F2 0F 11 8C 24 d32
        u8(0xF2); u8(0x0F); u8(0x11); u8(0x8C); u8(0x24); u32(d);
    }
    void movsdMemRspXmm2(uint32_t d) { // F2 0F 11 94 24 d32
        u8(0xF2); u8(0x0F); u8(0x11); u8(0x94); u8(0x24); u32(d);
    }
    void movsdMemRspXmm3(uint32_t d) { // F2 0F 11 9C 24 d32
        u8(0xF2); u8(0x0F); u8(0x11); u8(0x9C); u8(0x24); u32(d);
    }
    void movsdXmm1Rip(const std::string& sym) { // F2 0F 10 0D rel32
        u8(0xF2); u8(0x0F); u8(0x10); u8(0x0D); disp32placeholder(sym);
    }
    // int ALU на слотах: add/sub rax,[rsp+d]; imul rax,[rsp+d]
    void addRaxMemRsp(uint32_t d) { u8(0x48); u8(0x03); u8(0x84); u8(0x24); u32(d); }
    void subRaxMemRsp(uint32_t d) { u8(0x48); u8(0x2B); u8(0x84); u8(0x24); u32(d); }
    void imulRaxMemRsp(uint32_t d) { u8(0x48); u8(0x0F); u8(0xAF); u8(0x84); u8(0x24); u32(d); }
    // сравнения: cmp + setcc (cc: E=94 NE=95 B=92 BE=96 A=97 AE=93 L=9C LE=9E G=9F GE=9D)
    void cmpRaxMemRsp(uint32_t d) { u8(0x48); u8(0x3B); u8(0x84); u8(0x24); u32(d); }
    void cmpClMemRsp(uint32_t d) { u8(0x38); u8(0x8C); u8(0x24); u32(d); }
    void setccAl(uint8_t cc) { u8(0x0F); u8(cc); u8(0xC0); }
    void setccCl(uint8_t cc) { u8(0x0F); u8(cc); u8(0xC1); }
    void movMemRspAl(uint32_t d) { u8(0x88); u8(0x84); u8(0x24); u32(d); }
    void movzxEaxMemRsp(uint32_t d) { u8(0x0F); u8(0xB6); u8(0x84); u8(0x24); u32(d); }
    // bool-логика на байтах (значения нормализованы 0/1)
    void andClMemRsp(uint32_t d) { u8(0x22); u8(0x8C); u8(0x24); u32(d); }
    void orClMemRsp(uint32_t d) { u8(0x0A); u8(0x8C); u8(0x24); u32(d); }
    void xorCl1() { u8(0x80); u8(0xF1); u8(0x01); }
    // pow-хелперы
    void testRcx() { u8(0x48); u8(0x85); u8(0xC9); }
    void testEdx() { u8(0x85); u8(0xD2); }
    void negRcx() { u8(0x48); u8(0xF7); u8(0xD9); }
    void imulRdxRax() { u8(0x48); u8(0x0F); u8(0xAF); u8(0xD0); }
    void decRcx() { u8(0x48); u8(0xFF); u8(0xC9); }
    void incRdx() { u8(0x48); u8(0xFF); u8(0xC2); }
    void movRaxRdx() { u8(0x48); u8(0x89); u8(0xD0); }
    void movRdx1() { u8(0x48); u8(0xC7); u8(0xC2); u32(1); }
    void xorEax() { u8(0x31); u8(0xC0); }
    void cmpRdxRcx() { u8(0x48); u8(0x39); u8(0xCA); }
    void divsdXmm1Xmm0() { u8(0xF2); u8(0x0F); u8(0x5E); u8(0xC8); }
    // --- helpers для рантайма ввода (__read_line) и кучи ---
    void addRdxMemRsp(uint32_t d) { u8(0x48); u8(0x03); u8(0x94); u8(0x24); u32(d); }
    void addMemRspRax(uint32_t d) { u8(0x48); u8(0x01); u8(0x84); u8(0x24); u32(d); }
    void movEaxImm(uint32_t v) { u8(0xB8); u32(v); }
    void subEaxMemRsp(uint32_t d) { u8(0x2B); u8(0x84); u8(0x24); u32(d); }
    void movR8dEax() { u8(0x41); u8(0x89); u8(0xC0); } // mov r8d,eax (REX.B -> r/m=r8d)
    void cmpRaxImm(uint32_t v) { u8(0x48); u8(0x3D); u32(v); }
    void cmpRaxRcx() { u8(0x48); u8(0x39); u8(0xC8); }
    void cmpRaxR8() { u8(0x4C); u8(0x39); u8(0xC0); } // cmp rax,r8 (REX.R -> reg=r8)
    void testEax() { u8(0x85); u8(0xC0); }
    void cmpByteMemRsi(uint8_t v) { u8(0x80); u8(0x3E); u8(v); }
    void cmpByteMemRax(uint8_t v) { u8(0x80); u8(0x38); u8(v); }
    void incRsi() { u8(0x48); u8(0xFF); u8(0xC6); }
    void addRaxRdx() { u8(0x48); u8(0x01); u8(0xD0); }
    void addRcxImm(uint32_t v) { u8(0x48); u8(0x81); u8(0xC1); u32(v); }
    void addRdxImm(uint32_t v) { u8(0x48); u8(0x81); u8(0xC2); u32(v); }
    void andRdxImm(uint32_t v) { u8(0x48); u8(0x81); u8(0xE2); u32(v); }
    void movRdiRax() { u8(0x48); u8(0x89); u8(0xC7); }
    void movR8Rdx() { u8(0x49); u8(0x89); u8(0xD0); } // mov r8,rdx (REX.B -> r/m=r8)
    void movRcxR8() { u8(0x4C); u8(0x89); u8(0xC1); }
    void movRaxRdi() { u8(0x48); u8(0x89); u8(0xF8); }
    void movRdxR8() { u8(0x4C); u8(0x89); u8(0xC2); }
    void movRaxRip(const std::string& sym) {
        u8(0x48); u8(0x8B); u8(0x05); disp32placeholder(sym);
    }
    void movRipRax(const std::string& sym) {
        u8(0x48); u8(0x89); u8(0x05); disp32placeholder(sym);
    }
    void repMovsb() { u8(0xFC); u8(0xF3); u8(0xA4); } // cld; rep movsb (DF сброшен явно)
    void subRdxRcx() { u8(0x48); u8(0x29); u8(0xCA); }
    void testRdx() { u8(0x48); u8(0x85); u8(0xD2); }
    void decRdx() { u8(0x48); u8(0xFF); u8(0xCA); }
    void addRsiRdx() { u8(0x48); u8(0x01); u8(0xD6); }
    void movEaxMemRsp(uint32_t d) { u8(0x8B); u8(0x84); u8(0x24); u32(d); }
    // str ==/!=: байтовый цикл по [rsi+rdx] vs [rdi+rdx]
    void movAlSib() { u8(0x8A); u8(0x04); u8(0x16); }
    void cmpAlSib() { u8(0x3A); u8(0x04); u8(0x17); }

private:
    int nextLabel_ = 1;
    std::map<int, std::vector<uint32_t>> patches_;
    std::map<int, uint32_t> bound_;
};

// .rdata конструктор: склейка байтов + именованные смещения.
struct RData {
    std::vector<uint8_t> bytes;
    std::map<std::string, uint32_t> syms; // имя -> offset
    std::map<std::string, uint32_t> sizes;
    uint32_t add(const std::string& name, const void* p, size_t n) {
        uint32_t off = (uint32_t)bytes.size();
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) bytes.push_back(b[i]);
        syms[name] = off; sizes[name] = (uint32_t)n;
        return off;
    }
    uint32_t addStr(const std::string& name, const std::string& s) {
        return add(name, s.data(), s.size());
    }
};
