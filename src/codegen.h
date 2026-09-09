#pragma once
// Codegen: IR -> COFF-объекты (машинный код x64, байты, без LLVM/ASM).
//  - emitAppObj: пользовательский модуль -> app.obj (символ main,
//    ссылки на print_* и строки через REL32-релокации).
//  - emitBuiltinsObj: stdlib/builtins -> builtins.obj (символы
//    __write_stdout, print_str/int/float/bool/char + импорты
//    __imp_GetStdHandle/__imp_WriteFile).
// Linker затем связывает app.obj + builtins.obj в PE .exe.
#include "coff.h"
#include "ir.h"

CoffObject emitAppObj(const IRModule& m);
CoffObject emitBuiltinsObj();
