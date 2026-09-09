#include "lexer.h"
#include <cctype>
#include <stdexcept>

static void lexError(int line, int col, const std::string& msg) {
    throw std::runtime_error("lexer error at " + std::to_string(line) + ":" +
                             std::to_string(col) + ": " + msg);
}

std::string tokKindName(TokKind k) {
    switch (k) {
        case TokKind::Fn: return "fn";
        case TokKind::Const: return "const";
        case TokKind::If: return "if";
        case TokKind::Elif: return "elif";
        case TokKind::Else: return "else";
        case TokKind::While: return "while";
        case TokKind::For: return "for";
        case TokKind::In: return "in";
        case TokKind::Return: return "return";
        case TokKind::Import: return "import";
        case TokKind::Break: return "break";
        case TokKind::Continue: return "continue";
        case TokKind::True: return "true";
        case TokKind::False: return "false";
        case TokKind::Ident: return "ident";
        case TokKind::IntLit: return "int";
        case TokKind::FloatLit: return "float";
        case TokKind::StringLit: return "string";
        case TokKind::CharLit: return "char";
        case TokKind::LParen: return "(";
        case TokKind::RParen: return ")";
        case TokKind::Colon: return ":";
        case TokKind::Comma: return ",";
        case TokKind::Assign: return "=";
        case TokKind::Plus: return "+";
        case TokKind::Minus: return "-";
        case TokKind::Star: return "*";
        case TokKind::Slash: return "/";
        case TokKind::StarStar: return "**";
        case TokKind::Mod: return "%";
        case TokKind::EqEq: return "==";
        case TokKind::NotEq: return "!=";
        case TokKind::Lt: return "<";
        case TokKind::LtEq: return "<=";
        case TokKind::Gt: return ">";
        case TokKind::GtEq: return ">=";
        case TokKind::Bang: return "!";
        case TokKind::AmpAmp: return "&&";
        case TokKind::PipePipe: return "||";
        case TokKind::DotDot: return "..";
        case TokKind::DotDotEq: return "..=";
        case TokKind::Newline: return "newline";
        case TokKind::Indent: return "indent";
        case TokKind::Dedent: return "dedent";
        case TokKind::Eof: return "eof";
    }
    return "?";
}

// Лексинг одной содержательной строки (без ведущего отступа).
static void lexLine(const std::string& line, int lineNo, int baseCol,
                    std::vector<Token>& out) {
    size_t i = 0;
    int col = baseCol;
    auto push = [&](TokKind k) {
        Token t; t.kind = k; t.line = lineNo; t.col = col;
        out.push_back(t);
    };
    auto pushAt = [&](TokKind k, int ln, int cl) {
        Token t; t.kind = k; t.line = ln; t.col = cl;
        out.push_back(t);
    };
    while (i < line.size()) {
        char c = line[i];
        // comments: // to end of line, # to end of line
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (c == '#') break;
        if (c == ' ' || c == '\t') { i++; col++; continue; }
        if (c == '(') { push(TokKind::LParen); i++; col++; continue; }
        if (c == ')') { push(TokKind::RParen); i++; col++; continue; }
        if (c == ':') { push(TokKind::Colon); i++; col++; continue; }
        if (c == ',') { push(TokKind::Comma); i++; col++; continue; }
        if (c == '.') {
            if (i + 1 < line.size() && line[i + 1] == '.') {
                if (i + 2 < line.size() && line[i + 2] == '=') {
                    push(TokKind::DotDotEq); i += 3; col += 3; continue;
                }
                push(TokKind::DotDot); i += 2; col += 2; continue;
            }
            lexError(lineNo, col, "unexpected '.' (did you mean '..' for range?)");
        }
        if (c == '=') {
            if (i + 1 < line.size() && line[i + 1] == '=') {
                push(TokKind::EqEq); i += 2; col += 2; continue;
            }
            push(TokKind::Assign); i++; col++; continue;
        }
        if (c == '!') {
            if (i + 1 < line.size() && line[i + 1] == '=') {
                push(TokKind::NotEq); i += 2; col += 2; continue;
            }
            push(TokKind::Bang); i++; col++; continue;
        }
        if (c == '<') {
            if (i + 1 < line.size() && line[i + 1] == '=') {
                push(TokKind::LtEq); i += 2; col += 2; continue;
            }
            push(TokKind::Lt); i++; col++; continue;
        }
        if (c == '>') {
            if (i + 1 < line.size() && line[i + 1] == '=') {
                push(TokKind::GtEq); i += 2; col += 2; continue;
            }
            push(TokKind::Gt); i++; col++; continue;
        }
        if (c == '&') {
            if (i + 1 < line.size() && line[i + 1] == '&') {
                push(TokKind::AmpAmp); i += 2; col += 2; continue;
            }
            lexError(lineNo, col, "single '&' not supported, use '&&'");
        }
        if (c == '|') {
            if (i + 1 < line.size() && line[i + 1] == '|') {
                push(TokKind::PipePipe); i += 2; col += 2; continue;
            }
            lexError(lineNo, col, "single '|' not supported, use '||'");
        }
        if (c == '+') { push(TokKind::Plus); i++; col++; continue; }
        if (c == '-') { push(TokKind::Minus); i++; col++; continue; }
        if (c == '*') {
            if (i + 1 < line.size() && line[i + 1] == '*') {
                push(TokKind::StarStar); i += 2; col += 2; continue;
            }
            push(TokKind::Star); i++; col++; continue;
        }
        if (c == '/') { push(TokKind::Slash); i++; col++; continue; }
        if (c == '%') { push(TokKind::Mod); i++; col++; continue; }
        if (c == '"') {
            int scol = col;
            i++; col++;
            std::string s;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char e = line[i + 1];
                    if (e == 'n') s += '\n';
                    else if (e == 't') s += '\t';
                    else if (e == 'r') s += '\r';
                    else if (e == '\\') s += '\\';
                    else if (e == '"') s += '"';
                    else if (e == '0') s += '\0';
                    else s += e;
                    i += 2; col += 2;
                } else {
                    s += line[i]; i++; col++;
                }
            }
            if (i >= line.size()) lexError(lineNo, scol, "unterminated string");
            i++; col++; // closing "
            pushAt(TokKind::StringLit, lineNo, scol);
            out.back().text = s;
            continue;
        }
        if (c == '\'') {
            int scol = col;
            i++; col++;
            if (i >= line.size()) lexError(lineNo, scol, "unterminated char");
            char v = line[i];
            if (v == '\\' && i + 1 < line.size()) {
                char e = line[i + 1];
                if (e == 'n') v = '\n';
                else if (e == 't') v = '\t';
                else if (e == 'r') v = '\r';
                else if (e == '\\') v = '\\';
                else if (e == '\'') v = '\'';
                else if (e == '0') v = '\0';
                else v = e;
                i += 2; col += 2;
            } else {
                i++; col++;
            }
            if (i >= line.size() || line[i] != '\'')
                lexError(lineNo, scol, "unterminated char, expected '");
            i++; col++;
            pushAt(TokKind::CharLit, lineNo, scol);
            out.back().cval = v;
            out.back().text = std::string(1, v);
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
            bool isFloat = (j < line.size() && line[j] == '.' &&
                            j + 1 < line.size() && std::isdigit((unsigned char)line[j + 1]));
            int tcol = col;
            if (isFloat) {
                size_t k = j + 1;
                while (k < line.size() && std::isdigit((unsigned char)line[k])) k++;
                pushAt(TokKind::FloatLit, lineNo, tcol);
                out.back().text = line.substr(i, k - i);
                out.back().fval = std::stod(out.back().text);
                col += (int)(k - i); i = k;
            } else {
                pushAt(TokKind::IntLit, lineNo, tcol);
                out.back().text = line.substr(i, j - i);
                try { out.back().ival = std::stoll(out.back().text); }
                catch (...) { lexError(lineNo, tcol, "bad integer literal"); }
                col += (int)(j - i); i = j;
            }
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < line.size() &&
                   (std::isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
            std::string w = line.substr(i, j - i);
            int tcol = col;
            TokKind k = TokKind::Ident;
            if (w == "fn") k = TokKind::Fn;
            else if (w == "const") k = TokKind::Const;
            else if (w == "if") k = TokKind::If;
            else if (w == "elif") k = TokKind::Elif;
            else if (w == "else") k = TokKind::Else;
            else if (w == "while") k = TokKind::While;
            else if (w == "for") k = TokKind::For;
            else if (w == "in") k = TokKind::In;
            else if (w == "return") k = TokKind::Return;
            else if (w == "import") k = TokKind::Import;
            else if (w == "break") k = TokKind::Break;
            else if (w == "continue") k = TokKind::Continue;
            else if (w == "true") k = TokKind::True;
            else if (w == "false") k = TokKind::False;
            pushAt(k, lineNo, tcol);
            out.back().text = w;
            col += (int)(j - i); i = j;
            continue;
        }
        lexError(lineNo, col, std::string("unexpected char '") + c + "'");
    }
}

std::vector<Token> lex(const std::string& src, const std::string& fname) {
    (void)fname;
    std::vector<Token> out;
    // разбивка на строки
    std::vector<std::string> lines;
    {
        size_t s = 0;
        while (true) {
            size_t p = src.find('\n', s);
            if (p == std::string::npos) { lines.push_back(src.substr(s)); break; }
            lines.push_back(src.substr(s, p - s));
            s = p + 1;
        }
    }
    auto emit = [&](TokKind k, int ln, int cl) {
        Token t; t.kind = k; t.line = ln; t.col = cl;
        out.push_back(t);
    };
    std::vector<int> stack;
    stack.push_back(0);
    int lineNo = 0;
    for (std::string raw : lines) {
        lineNo++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        size_t k = 0;
        int indent = 0;
        while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) {
            if (raw[k] == ' ') indent += 1;
            else indent += 8 - (indent % 8);
            k++;
        }
        std::string rest = raw.substr(k);
        // пустые строки и строки-комментарии — вне системы отступов
        if (rest.empty() || rest[0] == '#' ||
            (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/'))
            continue;
        if (indent > stack.back()) {
            stack.push_back(indent);
            emit(TokKind::Indent, lineNo, 1);
        } else {
            while (indent < stack.back()) {
                stack.pop_back();
                emit(TokKind::Dedent, lineNo, 1);
            }
            if (indent != stack.back())
                lexError(lineNo, 1, "inconsistent dedent: no matching indent level");
        }
        lexLine(rest, lineNo, (int)k + 1, out);
        emit(TokKind::Newline, lineNo, (int)raw.size() + 1);
    }
    while (stack.size() > 1) {
        stack.pop_back();
        emit(TokKind::Dedent, lineNo + 1, 1);
    }
    emit(TokKind::Eof, lineNo + 1, 1);
    return out;
}
