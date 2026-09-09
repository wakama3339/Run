#pragma once
// Linker: COFF .obj -> PE32+ .exe.
// Вход: app.obj (main) + builtins.obj (print_*). Выход: консольный EXE x64.
// Импорты: только kernel32.dll (GetStdHandle, WriteFile, ExitProcess).
// Точка входа: символ main (завершается через ExitProcess, CRT не нужен).
#include <string>
#include <vector>

void linkExe(const std::vector<std::string>& objPaths,
             const std::string& exePath,
             const std::string& entrySymbol);
