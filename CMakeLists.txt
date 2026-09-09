cmake_minimum_required(VERSION 3.20)
project(runc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Компилятор: source (.rn) -> IR -> COFF .obj -> PE .exe
# Никаких LLVM / .s / .asm / внешних ассемблеров: машинный код x64
# эмитится байтами из src/x64.cpp + src/codegen.cpp,
# объекты пишутся src/coff.cpp, линкуются src/linker.cpp.

add_executable(runc
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ir.cpp
    src/x64.cpp
    src/coff.cpp
    src/codegen.cpp
    src/linker.cpp
)

target_include_directories(runc PRIVATE src)

if (MINGW OR WIN32)
    target_link_libraries(runc PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
