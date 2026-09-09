#pragma once
// COFF: запись .obj (x64, AMD64). Подмножество формата, достаточное
// для нашего backend + linker: секции .text/.rdata, REL32-релокации,
// таблица символов, строковая таблица. Длинные имена (>8) — через
// строковую таблицу (offset).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct CoffSymbol {
    std::string name;
    uint32_t value = 0;      // offset в секции
    int16_t section = 0;     // 1=.text 2=.rdata 0=UNDEF
    uint16_t type = 0;       // 0x20=function, 0=data
    uint8_t storage = 2;     // 2=EXTERNAL
};

struct CoffReloc {
    uint32_t offset = 0;     // в своей секции
    uint32_t symIndex = 0;   // индекс в таблице символов (заполняется writer'ом по имени)
    std::string symName;     // имя цели (для writer)
    uint16_t type = 4;       // REL32
};

struct CoffObject {
    std::vector<uint8_t> text;
    std::vector<CoffReloc> textRelocs;
    std::map<std::string, uint32_t> textSyms;  // func -> offset

    std::vector<uint8_t> rdata;
    std::map<std::string, uint32_t> rdataSyms; // datum -> offset

    std::vector<uint8_t> data;                 // .data (RW); релокаций нет
    std::map<std::string, uint32_t> dataSyms;  // datum -> offset
    uint32_t dataVirtualExtra = 0; // BSS-хвост: vsize = data.size()+extra,
                                   // в файле лежат только data.size() байт

    std::vector<std::string> undefined; // extern (funcs + __imp_*)
};

void writeCoffObj(const std::string& path, const CoffObject& obj);

// --- для linker'а: чтение наших .obj ---
struct ParsedObj {
    std::vector<uint8_t> text, rdata, data;
    struct Sym { std::string name; uint32_t value; int16_t sec; uint8_t cls; };
    std::vector<Sym> syms;
    struct Rel { uint32_t off; uint32_t sym; uint16_t type; int sec; }; // sec:1=text
    std::vector<Rel> rels; // только .text REL32 (rdata/data без релокаций)
    uint32_t dataVSize = 0; // vsize .data (может быть > data.size() из-за BSS)
};
ParsedObj parseCoffObj(const std::string& path);
