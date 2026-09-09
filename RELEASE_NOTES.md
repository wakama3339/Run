# runc v0.9

Самописный компилятор: исходники `.rn` → IR → COFF `.obj` → PE `.exe`, без LLVM/ASM. Только Windows x64 + `kernel32.dll`.

В архиве: `runc.exe`, `stdlib/`, `examples/*.rn`, `README.md`.

## Использование

```
runc.exe examples\hello.rn -o hello.exe
hello.exe
```

## Язык v0.9

- `print` (str/int/float/bool/char), `input()` / `input(prompt)`
- Переменные `x: T`, константы `const x: T`
- Функции с параметрами и `return`
- Выражения: `+ - * / % **`, сравнения, `! && ||`
- `if/elif/else`, `while`, `for x in a..b` / `a..=b`, `break`/`continue`
- `import "file.rn"` (подпапки и `../` поддерживаются)

## Конвейер

`.rn` → лексер (INDENT/DEDENT) → парсер → AST → IR (Module/Function/Block/Instr) → машинный код x64 → COFF-объекты (`app.obj` + `builtins.obj`) → собственный линкер → PE32+ `.exe`.
