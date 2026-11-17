#pragma once
// Idioma da interface (Ajustes do Sistema > Geral > Idioma): `idioma=pt` (padrão) ou `idioma=en` em
// %APPDATA%\Shell\ajustes.txt. O texto fica escrito em português no código e passa por Tr():
//   Tr(L"Abrir")                 -> L"Open" com idioma=en, L"Abrir" com idioma=pt
//   swprintf(buf, Tr(L"%d itens"), n)  -> o formato traduzido tem os mesmos %d/%ls, na mesma ordem
// O dicionário pt -> en fica em idioma_en.inc (compilado junto). Texto sem tradução sai em português.
// Os apps leem o idioma uma vez (na primeira chamada); o Shell chama idioma::Reload() quando o
// Ajustes avisa (ShellAjustesMudaram) e redesenha.
#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace idioma {

inline int g_en = -1;  // -1 = ainda não leu o ajustes.txt

// Lê `idioma=` em %APPDATA%\Shell\ajustes.txt (sem shell32: a DLL das janelas também usa isto).
inline bool ReadFile() {
    wchar_t ad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(ad) + L"\\Shell\\ajustes.txt").c_str(), L"r, ccs=UTF-8") || !f) return false;
    bool en = false;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        if (wcsncmp(line, L"idioma=", 7) == 0) en = line[7] == L'e' && line[8] == L'n';
    }
    fclose(f);
    return en;
}

inline bool En() {
    if (g_en < 0) g_en = ReadFile() ? 1 : 0;
    return g_en == 1;
}
inline void Set(bool en) { g_en = en ? 1 : 0; }
inline void Reload() { g_en = -1; }

struct Pair { const wchar_t* pt; const wchar_t* en; };

inline const std::unordered_map<std::wstring_view, const wchar_t*>& Map() {
    static const Pair table[] = {
#include "idioma_en.inc"
    };
    static const std::unordered_map<std::wstring_view, const wchar_t*> m = [] {
        std::unordered_map<std::wstring_view, const wchar_t*> r;
        r.reserve(sizeof(table) / sizeof(table[0]) * 2);
        for (auto& p : table) r.emplace(p.pt, p.en);
        return r;
    }();
    return m;
}

}  // namespace idioma

// Texto da interface no idioma escolhido. O ponteiro devolvido vale enquanto o argumento valer.
inline const wchar_t* Tr(const wchar_t* pt) {
    if (!pt || !idioma::En()) return pt;
    auto& m = idioma::Map();
    auto it = m.find(pt);
    return it == m.end() ? pt : it->second;
}
inline std::wstring Tr(const std::wstring& pt) {
    if (!idioma::En()) return pt;
    auto& m = idioma::Map();
    auto it = m.find(pt);
    return it == m.end() ? pt : std::wstring(it->second);
}
// Escolhe entre os dois textos sem dicionário (nomes de apps e afins).
inline const wchar_t* TrPick(const wchar_t* pt, const wchar_t* en) { return idioma::En() ? en : pt; }
