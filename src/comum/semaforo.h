#pragma once
// Cores dos botões da janela (fechar, minimizar, zoom). Com a cor de destaque Grafite (Ajustes > Aparência,
// `destaque=grafite` em %APPDATA%\Shell\ajustes.txt) os três ficam cinza, como no Mac.
// Uso: static const D2D1_COLOR_F fill_c[3] = {...}; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
// O arquivo é relido quando muda (no máximo a cada 0,5 s): trocar a cor no Ajustes chega no próximo desenho.
#include <windows.h>
#include <d2d1.h>
#include <cstdio>
#include <cwchar>
#include <string>

namespace semaforo {

enum Kind { FILL, RING, GLYPH };

inline bool Grafite() {
    static bool on = false;
    static DWORD lastCheck = 0;
    static FILETIME lastWrite{};
    DWORD now = GetTickCount();
    if (lastCheck && now - lastCheck < 500) return on;
    lastCheck = now;
    wchar_t ad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH) return on;
    std::wstring path = std::wstring(ad) + L"\\Shell\\ajustes.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return on;
    if (!CompareFileTime(&fa.ftLastWriteTime, &lastWrite)) return on;
    lastWrite = fa.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return on;
    bool g = false;
    wchar_t line[512];
    while (fgetws(line, 512, f)) if (wcsncmp(line, L"destaque=", 9) == 0) g = wcsncmp(line + 9, L"grafite", 7) == 0;
    fclose(f);
    on = g;
    return on;
}

inline const D2D1_COLOR_F* Pick(const D2D1_COLOR_F* normal, Kind k) {
    if (!Grafite()) return normal;
    static const D2D1_COLOR_F fill[3] = { { 0.58f, 0.58f, 0.59f, 1 }, { 0.58f, 0.58f, 0.59f, 1 }, { 0.58f, 0.58f, 0.59f, 1 } };
    static const D2D1_COLOR_F ring[3] = { { 0.47f, 0.47f, 0.48f, 1 }, { 0.47f, 0.47f, 0.48f, 1 }, { 0.47f, 0.47f, 0.48f, 1 } };
    static const D2D1_COLOR_F glyph[3] = { { 0.20f, 0.20f, 0.21f, 1 }, { 0.20f, 0.20f, 0.21f, 1 }, { 0.20f, 0.20f, 0.21f, 1 } };
    return k == FILL ? fill : k == RING ? ring : glyph;
}

inline D2D1_COLOR_F Close() { static const D2D1_COLOR_F c[3] = { { 1.0f, 0.373f, 0.341f, 1 } }; return Pick(c, FILL)[0]; }

}  // namespace semaforo
