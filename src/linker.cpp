#include "linker.h"
#include "coff.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>

static uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

#pragma pack(push, 1)
struct DosHdr {
    uint16_t magic = 0x5A4D;
    uint8_t pad[58] = {0};
    uint32_t lfanew = 0x80;
};
struct PeCoffHdr {
    uint32_t sig = 0x00004550;
    uint16_t machine = 0x8664;
    uint16_t nsec = 3;
    uint32_t stamp = 0;
    uint32_t symTab = 0;
    uint32_t nSym = 0;
    uint16_t optSize = 240;
    uint16_t chars = 0x002F; // executable | large address aware
};
struct OptHdr {
    uint16_t magic = 0x020B;
    uint8_t majorLink = 14, minorLink = 0;
    uint32_t codeSize = 0, dataSize = 0, bssSize = 0;
    uint32_t entry = 0, codeBase = 0x1000;
    uint64_t imageBase = 0x140000000ull;
    uint32_t sectAlign = 0x1000, fileAlign = 0x200;
    uint16_t osMaj = 6, osMin = 0, imgMaj = 0, imgMin = 0, subMaj = 6, subMin = 0;
    uint32_t win32ver = 0, imageSize = 0, hdrSize = 0, checksum = 0;
    uint16_t subsys = 3; // console
    uint16_t dllChars = 0x8160; // NX + DYNAMIC_BASE (ASLR)
    uint64_t stackRes = 0x100000, stackCom = 0x1000;
    uint64_t heapRes = 0x100000, heapCom = 0x1000;
    uint32_t loaderFlags = 0, numRva = 16;
    struct Dir { uint32_t rva = 0, size = 0; } dirs[16];
};
struct SecHdr {
    char name[8] = {0};
    uint32_t vsize = 0, vaddr = 0, rawSize = 0, rawPtr = 0;
    uint32_t reloc = 0, line = 0;
    uint16_t nreloc = 0, nline = 0;
    uint32_t chars = 0;
};
struct ImpDir {
    uint32_t ilt = 0, stamp = 0, fwd = 0, name = 0, iat = 0;
};
#pragma pack(pop)

void linkExe(const std::vector<std::string>& objPaths,
             const std::string& exePath,
             const std::string& entrySymbol) {
    if (objPaths.empty()) throw std::runtime_error("linker: no objects");
    std::vector<ParsedObj> objs;
    for (auto& p : objPaths) objs.push_back(parseCoffObj(p));

    // --- склейка секций ---
    std::vector<uint8_t> text, rdata, data;
    std::vector<uint32_t> textBase, rdataBase, dataBase;
    uint32_t dataExtraTotal = 0; // сумма BSS-хвостов .data
    for (auto& o : objs) {
        textBase.push_back((uint32_t)text.size());
        text.insert(text.end(), o.text.begin(), o.text.end());
        rdataBase.push_back((uint32_t)rdata.size());
        rdata.insert(rdata.end(), o.rdata.begin(), o.rdata.end());
        dataBase.push_back((uint32_t)data.size());
        data.insert(data.end(), o.data.begin(), o.data.end());
        if (o.dataVSize > o.data.size()) dataExtraTotal += o.dataVSize - (uint32_t)o.data.size();
    }
    uint32_t dataVSize = (uint32_t)data.size() + dataExtraTotal;

    // --- глобальная таблица определений ---
    struct Def { int sec; uint32_t off; }; // sec: 1=text 2=rdata 3=data
    std::map<std::string, Def> defs;
    for (size_t k = 0; k < objs.size(); k++) {
        for (auto& s : objs[k].syms) {
            if (s.sec == 1) {
                if (defs.count(s.name))
                    throw std::runtime_error("linker: duplicate symbol '" + s.name + "'");
                defs[s.name] = {1, textBase[k] + s.value};
            } else if (s.sec == 2) {
                if (defs.count(s.name))
                    throw std::runtime_error("linker: duplicate symbol '" + s.name + "'");
                defs[s.name] = {2, rdataBase[k] + s.value};
            } else if (s.sec == 3) {
                if (defs.count(s.name))
                    throw std::runtime_error("linker: duplicate symbol '" + s.name + "'");
                defs[s.name] = {3, dataBase[k] + s.value};
            }
        }
    }
    auto itE = defs.find(entrySymbol);
    if (itE == defs.end() || itE->second.sec != 1)
        throw std::runtime_error("linker: entry symbol '" + entrySymbol + "' not found");

    // --- импорты: все неопределённые __imp_X -> kernel32!X ---
    std::vector<std::string> impNames; // без префикса
    for (size_t k = 0; k < objs.size(); k++)
        for (auto& s : objs[k].syms)
            if (s.sec == 0 && s.name.rfind("__imp_", 0) == 0) {
                std::string api = s.name.substr(6);
                if (std::find(impNames.begin(), impNames.end(), api) == impNames.end())
                    impNames.push_back(api);
            }
    std::sort(impNames.begin(), impNames.end());
    for (auto& a : impNames)
        if (a != "GetStdHandle" && a != "WriteFile" && a != "ExitProcess" && a != "ReadFile")
            throw std::runtime_error("linker: unsupported import '" + a +
                                     "' (v0.9: only kernel32 GetStdHandle/WriteFile/ReadFile/ExitProcess)");

    // --- проверка остальных undefined ---
    for (size_t k = 0; k < objs.size(); k++)
        for (auto& s : objs[k].syms)
            if (s.sec == 0 && s.name.rfind("__imp_", 0) != 0 && !defs.count(s.name))
                throw std::runtime_error("linker: undefined symbol '" + s.name + "'");

    // --- раскладка PE ---
    const uint32_t SECT = 0x1000, FILEA = 0x200;
    uint32_t textRVA = SECT;
    uint32_t rdataRVA = alignUp(textRVA + (uint32_t)text.size(), SECT);
    // .idata строится ниже; сначала оценим размер
    // hint/name: 2 + len + 1, дополнить до чётного
    std::vector<uint32_t> hnSize;
    for (auto& a : impNames) {
        uint32_t s = 2 + (uint32_t)a.size() + 1;
        if (s % 2) s++;
        hnSize.push_back(s);
    }
    uint32_t nImp = (uint32_t)impNames.size();
    uint32_t idataDir = 40;                                  // ImpDir + null
    uint32_t idataILT = idataDir;                            // INT
    uint32_t idataIAT = idataILT + (nImp + 1) * 8;           // IAT
    uint32_t idataHN = idataIAT + (nImp + 1) * 8;            // hint/name'ы
    uint32_t idataDll = idataHN;
    for (auto s : hnSize) idataDll += s;
    uint32_t idataSize = idataDll + 13; // "kernel32.dll\0"
    uint32_t dataRVA = alignUp(rdataRVA + (uint32_t)rdata.size(), SECT);
    uint32_t idataRVA = alignUp(dataRVA + dataVSize, SECT);

    uint32_t imageSize = alignUp(idataRVA + idataSize, SECT);
    uint32_t hdrSize = alignUp(0x80 + 4 + 20 + 240 + 4 * 40, FILEA);
    uint32_t textRaw = hdrSize;
    uint32_t rdataRaw = textRaw + alignUp((uint32_t)text.size(), FILEA);
    uint32_t dataRaw = rdataRaw + alignUp((uint32_t)data.size(), FILEA);
    uint32_t idataRaw = dataRaw + alignUp((uint32_t)data.size(), FILEA);
    uint32_t fileSize = idataRaw + alignUp(idataSize, FILEA);

    // --- .idata байты ---
    std::vector<uint8_t> idata(idataSize, 0);
    auto w32 = [&](std::vector<uint8_t>& b, uint32_t at, uint32_t v) {
        b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF;
        b[at + 2] = (v >> 16) & 0xFF; b[at + 3] = (v >> 24) & 0xFF;
    };
    auto w64 = [&](std::vector<uint8_t>& b, uint32_t at, uint64_t v) {
        for (int i = 0; i < 8; i++) b[at + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    };
    const char* dll = "kernel32.dll";
    // ImpDir[0]
    w32(idata, 0, idataRVA + idataILT);
    w32(idata, 12, idataRVA + idataDll);
    w32(idata, 16, idataRVA + idataIAT);
    // INT + IAT + hint/name
    std::map<std::string, uint32_t> impIAT; // api -> RVA слота IAT
    uint32_t hnAt = idataHN;
    for (size_t i = 0; i < impNames.size(); i++) {
        w64(idata, idataILT + (uint32_t)i * 8, idataRVA + hnAt);
        w64(idata, idataIAT + (uint32_t)i * 8, idataRVA + hnAt);
        impIAT[impNames[i]] = idataRVA + idataIAT + (uint32_t)i * 8;
        idata[hnAt] = 0; idata[hnAt + 1] = 0; // hint
        memcpy(idata.data() + hnAt + 2, impNames[i].c_str(), impNames[i].size() + 1);
        hnAt += hnSize[i];
    }
    memcpy(idata.data() + idataDll, dll, 13);

    // --- VA резолвер ---
    auto symVA = [&](const std::string& n) -> uint64_t {
        const uint64_t BASE = 0x140000000ull;
        if (n.rfind("__imp_", 0) == 0) {
            auto it = impIAT.find(n.substr(6));
            if (it == impIAT.end()) throw std::runtime_error("linker: bad imp " + n);
            return BASE + it->second;
        }
        auto it = defs.find(n);
        if (it == defs.end()) throw std::runtime_error("linker: undef " + n);
        if (it->second.sec == 1) return BASE + textRVA + it->second.off;
        if (it->second.sec == 2) return BASE + rdataRVA + it->second.off;
        return BASE + dataRVA + it->second.off;
    };

    // --- применить REL32 ---
    const uint64_t BASE = 0x140000000ull;
    for (size_t k = 0; k < objs.size(); k++) {
        for (auto& r : objs[k].rels) {
            if (r.type != 4) throw std::runtime_error("linker: only REL32 supported");
            if (r.sym >= objs[k].syms.size())
                throw std::runtime_error("linker: bad reloc symbol index");
            std::string name = objs[k].syms[r.sym].name;
            uint64_t target = symVA(name);
            uint32_t patchMerged = textBase[k] + r.off;
            uint64_t nextIP = BASE + textRVA + patchMerged + 4;
            int64_t d = (int64_t)target - (int64_t)nextIP;
            if (d < INT32_MIN || d > INT32_MAX)
                throw std::runtime_error("linker: rel32 out of range for " + name);
            uint32_t v = (uint32_t)(int32_t)d;
            text[patchMerged] = v & 0xFF; text[patchMerged + 1] = (v >> 8) & 0xFF;
            text[patchMerged + 2] = (v >> 16) & 0xFF;
            text[patchMerged + 3] = (v >> 24) & 0xFF;
        }
    }

    // --- заголовки ---
    std::vector<uint8_t> file(fileSize, 0);
    auto fw32 = [&](uint32_t at, uint32_t v) {
        file[at] = v & 0xFF; file[at + 1] = (v >> 8) & 0xFF;
        file[at + 2] = (v >> 16) & 0xFF; file[at + 3] = (v >> 24) & 0xFF;
    };
    auto fw64 = [&](uint32_t at, uint64_t v) {
        for (int i = 0; i < 8; i++) file[at + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    };
    // DOS
    file[0] = 'M'; file[1] = 'Z';
    fw32(0x3C, 0x80);
    // PE COFF
    uint32_t po = 0x80;
    file[po] = 'P'; file[po + 1] = 'E'; file[po + 2] = 0; file[po + 3] = 0;
    po += 4;
    // machine=0x8664, nsec=4
    file[po] = 0x64; file[po + 1] = 0x86; file[po + 2] = 4; file[po + 3] = 0;
    // stamp/symtab/nsym/optsize/chars
    fw32(po + 4, 0); fw32(po + 8, 0); fw32(po + 12, 0);
    file[po + 16] = 240; file[po + 17] = 0;
    // 0x26 = EXECUTABLE | LINE_NUMS_STRIPPED | LARGE_ADDRESS_AWARE
    // (без RELOCS_STRIPPED: иначе лоадер Win10/11 + DYNAMIC_BASE может
    // отказаться грузить образ, т.к. ребазировать нечем).
    file[po + 18] = 0x26; file[po + 19] = 0x00;
    po += 20;
    // Optional
    uint32_t oo = po;
    file[oo] = 0x0B; file[oo + 1] = 0x02;
    file[oo + 2] = 14; file[oo + 3] = 0;
    fw32(oo + 4, alignUp((uint32_t)text.size(), SECT));   // codeSize
    fw32(oo + 8, alignUp((uint32_t)rdata.size(), SECT) + alignUp(dataVSize, SECT) +
                     alignUp(idataSize, SECT));
    fw32(oo + 12, 0);
    fw32(oo + 16, textRVA + itE->second.off);             // entry = main
    fw32(oo + 20, textRVA);                               // baseOfCode
    fw64(oo + 24, BASE);
    fw32(oo + 32, SECT); fw32(oo + 36, FILEA);
    // Версии (WORD'ы): OS 6.0, Image 0.0, Subsystem 6.0.
    // ВАЖНО: MajorSubsystemVersion = 0 отвергается лоадером Windows 10/11
    // (CreateProcess -> ERROR_BAD_EXE_FORMAT 193). Минимум — 5.x.
    file[oo + 40] = 6; file[oo + 41] = 0;  // MajorOSVersion
    file[oo + 42] = 0; file[oo + 43] = 0;  // MinorOSVersion
    file[oo + 44] = 0; file[oo + 45] = 0;  // MajorImageVersion
    file[oo + 46] = 0; file[oo + 47] = 0;  // MinorImageVersion
    file[oo + 48] = 6; file[oo + 49] = 0;  // MajorSubsystemVersion
    file[oo + 50] = 0; file[oo + 51] = 0;  // MinorSubsystemVersion
    fw32(oo + 52, 0);                                     // Win32VersionValue
    fw32(oo + 56, imageSize);                             // SizeOfImage
    fw32(oo + 60, hdrSize);                               // SizeOfHeaders
    fw32(oo + 64, 0);                                     // CheckSum
    file[oo + 68] = 3; file[oo + 69] = 0;                 // subsystem console
    file[oo + 70] = 0x60; file[oo + 71] = 0x01;           // dllChars NX|DYNAMIC_BASE|HIGH_ENTROPY
    fw64(oo + 72, 0x100000); fw64(oo + 80, 0x1000);
    fw64(oo + 88, 0x100000); fw64(oo + 96, 0x1000);
    fw32(oo + 104, 0); fw32(oo + 108, 16);
    // dir[0] EXPORT = null (нули)
    fw32(oo + 120, idataRVA); fw32(oo + 124, 40);         // dir[1] IMPORT
    // dir[2..11] = null (нули)
    fw32(oo + 208, idataRVA + idataIAT);
    fw32(oo + 212, (nImp + 1) * 8);                       // dir[12] IAT
    // остальные dirs нули
    po += 240;
    // Section headers
    auto sec = [&](int i, const char* n, uint32_t vs, uint32_t va, uint32_t rs,
                   uint32_t rp, uint32_t ch) {
        uint32_t at = po + i * 40;
        memset(file.data() + at, 0, 8);
        memcpy(file.data() + at, n, strlen(n));
        fw32(at + 8, vs); fw32(at + 12, va); fw32(at + 16, rs); fw32(at + 20, rp);
        fw32(at + 24, 0); fw32(at + 28, 0);
        file[at + 32] = 0; file[at + 33] = 0; file[at + 34] = 0; file[at + 35] = 0;
        fw32(at + 36, ch);
    };
    sec(0, ".text", (uint32_t)text.size(), textRVA, alignUp((uint32_t)text.size(), FILEA),
        textRaw, 0x60000020);
    sec(1, ".rdata", (uint32_t)rdata.size(), rdataRVA,
        alignUp((uint32_t)rdata.size(), FILEA), rdataRaw, 0x40000040);
    sec(2, ".data", dataVSize, dataRVA, alignUp((uint32_t)data.size(), FILEA), dataRaw,
        0xC0000040);
    sec(3, ".idata", idataSize, idataRVA, alignUp(idataSize, FILEA), idataRaw,
        0x40000040);
    // raw
    if (!text.empty()) memcpy(file.data() + textRaw, text.data(), text.size());
    if (!rdata.empty()) memcpy(file.data() + rdataRaw, rdata.data(), rdata.size());
    if (!data.empty()) memcpy(file.data() + dataRaw, data.data(), data.size());
    memcpy(file.data() + idataRaw, idata.data(), idata.size());

    std::ofstream f(exePath, std::ios::binary);
    if (!f) throw std::runtime_error("linker: cannot write " + exePath);
    f.write((char*)file.data(), file.size());
}
