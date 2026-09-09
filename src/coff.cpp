#include "coff.h"
#include <cstring>
#include <fstream>
#include <stdexcept>

#pragma pack(push, 1)
struct FileHeader {
    uint16_t machine = 0x8664;
    uint16_t numSections = 2;
    uint32_t timeStamp = 0;
    uint32_t symTable = 0;
    uint32_t numSyms = 0;
    uint16_t optSize = 0;
    uint16_t chars = 0;
};
struct SectHeader {
    char name[8] = {0};
    uint32_t vsize = 0;
    uint32_t vaddr = 0;
    uint32_t rawSize = 0;
    uint32_t rawPtr = 0;
    uint32_t relocPtr = 0;
    uint32_t linePtr = 0;
    uint16_t numReloc = 0;
    uint16_t numLine = 0;
    uint32_t chars = 0;
};
struct RelocEnt {
    uint32_t vaddr = 0;
    uint32_t sym = 0;
    uint16_t type = 0;
};
struct SymEnt {
    char name[8] = {0};
    uint32_t value = 0;
    int16_t sec = 0;
    uint16_t type = 0;
    uint8_t cls = 0;
    uint8_t aux = 0;
};
#pragma pack(pop)

static void putName(char dst[8], const std::string& n, std::vector<char>& strtab,
                    bool& isLong) {
    if (n.size() <= 8) {
        memset(dst, 0, 8);
        memcpy(dst, n.data(), n.size());
        isLong = false;
    } else {
        // 4 нуля + offset в string table (offset считается от начала
        // таблицы, включая 4 байта длины — поэтому +4).
        memset(dst, 0, 8);
        uint32_t off = 4 + (uint32_t)strtab.size();
        const char* s = n.c_str();
        for (size_t i = 0; i <= n.size(); i++) strtab.push_back(s[i]);
        memcpy(dst + 4, &off, 4);
        isLong = true;
    }
}

void writeCoffObj(const std::string& path, const CoffObject& obj) {
    // Таблица символов: textSyms(sec1) + rdataSyms(sec2) + dataSyms(sec3) + undefined(sec0)
    std::vector<CoffSymbol> syms;
    for (auto& kv : obj.textSyms) {
        CoffSymbol s; s.name = kv.first; s.value = kv.second;
        s.section = 1; s.type = 0x20; s.storage = 2;
        syms.push_back(s);
    }
    for (auto& kv : obj.rdataSyms) {
        CoffSymbol s; s.name = kv.first; s.value = kv.second;
        s.section = 2; s.type = 0; s.storage = 2;
        syms.push_back(s);
    }
    for (auto& kv : obj.dataSyms) {
        CoffSymbol s; s.name = kv.first; s.value = kv.second;
        s.section = 3; s.type = 0; s.storage = 2;
        syms.push_back(s);
    }
    // undefined без дублей, исключая уже определённые
    auto defined = [&](const std::string& n) {
        for (auto& s : syms) if (s.name == n) return true;
        return false;
    };
    for (auto& n : obj.undefined) {
        if (defined(n)) continue;
        bool dup = false;
        for (auto& s : syms) if (s.name == n) dup = true;
        if (dup) continue;
        CoffSymbol s; s.name = n; s.value = 0; s.section = 0; s.type = 0; s.storage = 2;
        syms.push_back(s);
    }
    if (obj.dataVirtualExtra > 0) {
        CoffSymbol s; s.name = "__data_vsize";
        s.value = (uint32_t)obj.data.size() + obj.dataVirtualExtra;
        s.section = -1; // IMAGE_SYM_ABSOLUTE
        s.type = 0; s.storage = 2;
        syms.push_back(s);
    }
    // индекс по имени
    std::map<std::string, uint32_t> idx;
    for (uint32_t i = 0; i < syms.size(); i++) idx[syms[i].name] = i;

    std::vector<char> strtab; // без первых 4 байт длины пока
    strtab.reserve(256);
    std::vector<SymEnt> sent(syms.size());
    std::vector<bool> isLong(syms.size(), false);
    // сначала резервируем место под короткие, длинные дописываем по ходу
    for (size_t i = 0; i < syms.size(); i++) {
        memset(sent[i].name, 0, 8);
        sent[i].value = syms[i].value;
        sent[i].sec = syms[i].section;
        sent[i].type = syms[i].type;
        sent[i].cls = syms[i].storage;
        sent[i].aux = 0;
    }
    for (size_t i = 0; i < syms.size(); i++) {
        bool l = false;
        putName(sent[i].name, syms[i].name, strtab, l);
        isLong[i] = l;
    }

    uint32_t textSize = (uint32_t)obj.text.size();
    uint32_t rdataSize = (uint32_t)obj.rdata.size();
    uint32_t dataSize = (uint32_t)obj.data.size();
    uint16_t nTextReloc = (uint16_t)obj.textRelocs.size();

    // layout файла
    uint32_t off = 0;
    uint32_t hdrOff = 0; off += sizeof(FileHeader);
    uint32_t secOff = off; off += 3 * sizeof(SectHeader);
    uint32_t textOff = off; off += textSize;
    uint32_t rdataOff = off; off += rdataSize;
    uint32_t dataOff = off; off += dataSize;
    uint32_t textRelOff = off; off += (uint32_t)obj.textRelocs.size() * sizeof(RelocEnt);
    uint32_t symOff = off; off += (uint32_t)sent.size() * sizeof(SymEnt);
    uint32_t strOff = off;
    uint32_t strSize = 4 + (uint32_t)strtab.size();
    off += strSize;

    std::vector<uint8_t> file(off, 0);
    auto w32 = [&](uint32_t at, uint32_t v) {
        file[at] = v & 0xFF; file[at + 1] = (v >> 8) & 0xFF;
        file[at + 2] = (v >> 16) & 0xFF; file[at + 3] = (v >> 24) & 0xFF;
    };
    auto w16 = [&](uint32_t at, uint16_t v) {
        file[at] = v & 0xFF; file[at + 1] = (v >> 8) & 0xFF;
    };

    FileHeader fh;
    fh.numSections = 3;
    fh.symTable = symOff;
    fh.numSyms = (uint32_t)sent.size();
    memcpy(file.data() + hdrOff, &fh, sizeof(fh));

    SectHeader s1, s2, s3;
    memcpy(s1.name, ".text", 5);
    s1.rawSize = textSize; s1.rawPtr = textSize ? textOff : 0;
    s1.relocPtr = nTextReloc ? textRelOff : 0;
    s1.numReloc = nTextReloc;
    s1.chars = 0x60500020u;
    memcpy(s2.name, ".rdata", 6);
    s2.rawSize = rdataSize; s2.rawPtr = rdataSize ? rdataOff : 0;
    s2.chars = 0x40300040u;
    memcpy(s3.name, ".data", 5);
    s3.rawSize = dataSize; s3.rawPtr = dataSize ? dataOff : 0;
    s3.chars = 0xC0300040u; // read+write
    // BSS-хвост (.data vsize > raw): extra передаём линкеру через ABSOLUTE
    // символ __data_vsize (ниже, вместе с undefined).
    memcpy(file.data() + secOff, &s1, sizeof(s1));
    memcpy(file.data() + secOff + sizeof(s1), &s2, sizeof(s2));
    memcpy(file.data() + secOff + 2 * sizeof(s1), &s3, sizeof(s3));

    if (textSize) memcpy(file.data() + textOff, obj.text.data(), textSize);
    if (rdataSize) memcpy(file.data() + rdataOff, obj.rdata.data(), rdataSize);
    if (dataSize) memcpy(file.data() + dataOff, obj.data.data(), dataSize);

    for (size_t i = 0; i < obj.textRelocs.size(); i++) {
        auto& r = obj.textRelocs[i];
        auto it = idx.find(r.symName);
        if (it == idx.end())
            throw std::runtime_error("coff: unknown reloc symbol '" + r.symName + "'");
        uint32_t at = textRelOff + (uint32_t)i * sizeof(RelocEnt);
        w32(at, r.offset); w32(at + 4, it->second); w16(at + 8, r.type);
    }
    if (!sent.empty()) memcpy(file.data() + symOff, sent.data(), sent.size() * sizeof(SymEnt));
    w32(strOff, strSize);
    if (!strtab.empty()) memcpy(file.data() + strOff + 4, strtab.data(), strtab.size());

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write((char*)file.data(), file.size());
}

static uint16_t r16(const std::vector<uint8_t>& b, size_t o) {
    return (uint16_t)(b[o] | (b[o + 1] << 8));
}
static uint32_t r32(const std::vector<uint8_t>& b, size_t o) {
    return (uint32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
}

ParsedObj parseCoffObj(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < sizeof(FileHeader)) throw std::runtime_error("bad obj " + path);
    uint16_t machine = r16(b, 0);
    if (machine != 0x8664) throw std::runtime_error("obj is not AMD64: " + path);
    uint16_t nsec = r16(b, 2);
    uint32_t symTab = r32(b, 8), numSyms = r32(b, 12);
    ParsedObj o;
    struct Sec { std::string name; uint32_t rawSize, rawPtr, relPtr; uint16_t nrel; };
    std::vector<Sec> secs;
    for (int i = 0; i < nsec; i++) {
        size_t so = 20 + i * 40;
        char nm[9] = {0}; memcpy(nm, b.data() + so, 8);
        Sec s; s.name = nm;
        s.rawSize = r32(b, so + 16); s.rawPtr = r32(b, so + 20);
        s.relPtr = r32(b, so + 24); s.nrel = r16(b, so + 32);
        secs.push_back(s);
    }
    // string table
    uint32_t strOff = symTab + numSyms * 18;
    uint32_t strSize = 0;
    if (strOff + 4 <= b.size()) strSize = r32(b, strOff);
    auto getStr = [&](uint32_t off) -> std::string {
        if (off < 4 || off >= strSize) return "";
        std::string s;
        for (uint32_t k = strOff + off; k < strOff + strSize && b[k]; k++) s += (char)b[k];
        return s;
    };
    o.dataVSize = 0;
    for (uint32_t i = 0; i < numSyms; i++) {
        size_t so = symTab + i * 18;
        ParsedObj::Sym s;
        uint32_t z = r32(b, so);
        if (z == 0) s.name = getStr(r32(b, so + 4));
        else { char nm[9] = {0}; memcpy(nm, b.data() + so, 8); s.name = nm; }
        s.value = r32(b, so + 8);
        s.sec = (int16_t)r16(b, so + 12);
        s.cls = b[so + 16];
        if (s.name == "__data_vsize" && s.sec == -1) {
            o.dataVSize = s.value; // BSS-хвост .data, не символ
        } else {
            o.syms.push_back(s);
        }
        uint8_t aux = b[so + 17];
        i += aux;
    }
    for (int si = 0; si < (int)secs.size(); si++) {
        auto& s = secs[si];
        std::vector<uint8_t>* dst = nullptr;
        if (s.name == ".text") dst = &o.text;
        else if (s.name == ".rdata") dst = &o.rdata;
        else if (s.name == ".data") dst = &o.data;
        if (dst && s.rawSize) {
            if (s.rawPtr + s.rawSize > b.size()) throw std::runtime_error("bad section " + path);
            dst->assign(b.begin() + s.rawPtr, b.begin() + s.rawPtr + s.rawSize);
        }
        for (int k = 0; k < s.nrel; k++) {
            size_t ro = s.relPtr + k * 10;
            ParsedObj::Rel r;
            r.off = r32(b, ro); r.sym = r32(b, ro + 4); r.type = r16(b, ro + 8);
            r.sec = si + 1;
            if (s.name == ".text") o.rels.push_back(r);
        }
    }
    if (o.dataVSize < o.data.size()) o.dataVSize = (uint32_t)o.data.size();
    return o;
}
