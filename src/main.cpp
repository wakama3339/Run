// runc: драйвер компилятора.
// Конвейер: .rn --lex/parse--> AST --buildIR--> IR --codegen--> .obj (COFF)
//          --link--> .exe (PE32+).
// stdlib/builtins.rn автоматически "берётся в себя" каждой программой.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "codegen.h"
#include "coff.h"
#include "ir.h"
#include "lexer.h"
#include "linker.h"
#include "parser.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

static std::string exeDir(const char* argv0) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n > 0) return fs::path(std::string(buf, n)).parent_path().string();
#endif
    try {
        return fs::absolute(fs::path(argv0)).parent_path().string();
    } catch (...) {
        return ".";
    }
}

static bool endsWithRn(const std::string& p) {
    return p.size() >= 3 && p.substr(p.size() - 3) == ".rn";
}

// Рекурсивный загрузчик модулей: import "a.rn" резолвится относительно
// директории импортирующего файла; повторы — один раз (по каноническому
// пути), циклы — ошибка. Возвращает слитую программу (сначала зависимости).
static Program loadModule(const std::string& path,
                          std::map<std::string, int>& state, // 1=в процессе, 2=готов
                          std::vector<std::string>& stack) {
    std::error_code ec;
    std::string canon = fs::weakly_canonical(fs::absolute(path, ec), ec).string();
    if (ec) throw std::runtime_error("cannot resolve import '" + path + "'");
    auto it = state.find(canon);
    if (it != state.end() && it->second == 2) return Program();
    if (it != state.end() && it->second == 1) {
        std::string chain;
        for (auto& s : stack) chain += s + " -> ";
        throw std::runtime_error("circular import: " + chain + canon);
    }
    state[canon] = 1;
    stack.push_back(canon);
    std::string src;
    try {
        src = readFile(canon);
    } catch (...) {
        throw std::runtime_error("cannot open import '" + canon + "' (imported from '" +
                                 (stack.size() >= 2 ? stack[stack.size() - 2] : "?") + "')");
    }
    Program prog;
    try {
        prog = parse(lex(src, canon), canon);
    } catch (std::exception& e) {
        throw std::runtime_error(canon + ": " + e.what());
    }
    Program out;
    std::string dir = fs::path(canon).parent_path().string();
    for (auto& imp : prog.imports) {
        if (!endsWithRn(imp))
            throw std::runtime_error(canon + ": import path must end with '.rn', got '" + imp +
                                     "' (use '/' separators)");
        Program sub = loadModule((fs::path(dir) / imp).string(), state, stack);
        for (auto& f : sub.funcs) out.funcs.push_back(std::move(f));
    }
    for (auto& f : prog.funcs) out.funcs.push_back(std::move(f));
    state[canon] = 2;
    stack.pop_back();
    return out;
}

static std::string findBuiltins(const std::string& exedir) {
    const char* cands[] = {
        "stdlib/builtins.rn",
        "../stdlib/builtins.rn",
    };
    std::vector<std::string> roots = {exedir, ".", ".."};
    // 1) рядом с exe
    for (auto& r : roots)
        for (auto& c : cands) {
            fs::path p = fs::path(r) / c;
            if (fs::exists(p)) return p.string();
        }
    // 2) имена из исходника выше по дереву (исходник в подпапке)
    return "";
}

static void usage() {
    std::cout << "runc v0.8 — IR -> COFF obj -> PE exe (x64, без LLVM/ASM)\n"
                 "usage: runc <file.rn> [-o out.exe] [--emit-ir] [--keep-objs] [-v]\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)0;
#else
    std::cerr << "runc v0.8: link target is Windows-x64 PE; build on Windows.\n";
    return 1;
#endif
    if (argc < 2) {
        usage();
        return 1;
    }
    std::string srcPath, outExe;
    bool emitIr = false, keepObjs = true, verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) outExe = argv[++i];
        else if (a == "--emit-ir") emitIr = true;
        else if (a == "--keep-objs") keepObjs = true;
        else if (a == "--drop-objs") keepObjs = false;
        else if (a == "-v") verbose = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != '-') srcPath = a;
        else {
            std::cerr << "unknown arg: " << a << "\n";
            return 1;
        }
    }
    if (srcPath.empty()) {
        usage();
        return 1;
    }
    try {
        fs::path src(srcPath);
        std::string stem = src.stem().string();
        fs::path outDir = src.has_parent_path() ? src.parent_path() : fs::path(".");
        if (outExe.empty()) outExe = (outDir / (stem + ".exe")).string();
        std::string appObj = (outDir / (stem + ".app.obj")).string();
        std::string bltObj = (outDir / "builtins.obj").string();

        auto log = [&](const std::string& s) {
            if (verbose) std::cout << s << "\n";
        };

        // 1. Модули: stdlib/builtins + пользовательский файл с import'ами.
        // Каждый import резолвится относительно своего файла; повторы —
        // один раз, циклы — ошибка. Все функции линкуются по именам.
        std::string exedir = exeDir(argv[0]);
        std::string bltPath = findBuiltins(exedir);
        Program prog;
        {
            std::map<std::string, int> state;
            std::vector<std::string> stack;
            if (!bltPath.empty()) {
                Program blt = loadModule(bltPath, state, stack);
                for (auto& f : blt.funcs) prog.funcs.push_back(std::move(f));
                log("[stdlib] included " + bltPath);
            } else {
                log("[stdlib] builtins.rn not found, continuing with intrinsic print");
            }
            Program user = loadModule(srcPath, state, stack);
            log("[front] user funcs: " + std::to_string(user.funcs.size()));
            for (auto& f : user.funcs) prog.funcs.push_back(std::move(f));
        }

        // 2. IR
        IRModule mod = buildIR(prog);
        if (emitIr) {
            std::cout << mod.dump();
        }

        // 3. COFF objects
        CoffObject app = emitAppObj(mod);
        CoffObject blt = emitBuiltinsObj();
        writeCoffObj(appObj, app);
        writeCoffObj(bltObj, blt);
        log("[coff] wrote " + appObj + " (text=" + std::to_string(app.text.size()) + "B)");
        log("[coff] wrote " + bltObj + " (text=" + std::to_string(blt.text.size()) + "B)");

        // 4. Link -> EXE
        linkExe({appObj, bltObj}, outExe, "main");
        std::cout << "built " << outExe << "\n";
        if (!keepObjs) {
            std::error_code ec;
            fs::remove(appObj, ec);
        }
        return 0;
    } catch (std::exception& e) {
        std::cerr << "runc: error: " << e.what() << "\n";
        return 1;
    }
}
