#pragma once
// Leitura e gravação do %APPDATA%\Shell\ajustes.txt pelo Shell e pelo Ajustes do Sistema (os dois regravam o arquivo
// inteiro). Duas armadilhas que este arquivo resolve:
// - linha de qualquer tamanho: apps_ordem e launchpad_ocultos guardam nomes de apps separados por "|" e passam
//   fácil de 512 caracteres; lidas com fgetws(512) viravam dois pedaços, o primeiro cortado, e a regravação
//   seguinte salvava a lista cortada (ordem do Launchpad perdida);
// - gravação atômica: com "w" direto, quem lesse no meio da gravação via o arquivo vazio (o Ajustes relê antes de
//   gravar e regravava só a chave dele, apagando o resto). Agora grava em .tmp e troca de uma vez.
#include <windows.h>
#include <cstdio>
#include <map>
#include <string>

namespace ajarq {

// uma linha inteira, sem \r\n no fim; false no fim do arquivo
inline bool LerLinha(FILE* f, std::wstring& s) {
    s.clear();
    wchar_t buf[1024];
    bool leu = false;
    while (fgetws(buf, (int)(sizeof(buf) / sizeof(buf[0])), f)) {
        leu = true;
        s += buf;
        if (!s.empty() && s.back() == L'\n') break;
    }
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return leu;
}

// chave=valor (linhas com # ou sem = ficam de fora); false se o arquivo não abriu
inline bool Ler(const std::wstring& path, std::map<std::wstring, std::wstring>& out) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"r, ccs=UTF-8") || !fp) return false;
    std::wstring s;
    while (LerLinha(fp, s)) {
        while (!s.empty() && s.back() == L' ') s.pop_back();
        size_t eq = s.find(L'=');
        if (eq != std::wstring::npos && !s.empty() && s[0] != L'#') out[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(fp);
    return true;
}

inline bool Gravar(const std::wstring& path, const std::map<std::wstring, std::wstring>& m) {
    std::wstring tmp = path + L".tmp";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, tmp.c_str(), L"w, ccs=UTF-8") || !fp) return false;
    for (auto& [k, v] : m) fwprintf(fp, L"%ls=%ls\n", k.c_str(), v.c_str());
    bool ok = fflush(fp) == 0;
    fclose(fp);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    // outro app pode estar com o arquivo aberto para ler (fopen não divide a exclusão): tenta de novo e, em último
    // caso, grava direto como antes
    for (int i = 0; i < 5; i++) {
        // sem MOVEFILE_WRITE_THROUGH: o Shell grava na thread da interface, e esperar o disco (ocupado, issue #32)
        // a travaria; a troca já é atômica para quem lê
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(10);
    }
    DeleteFileW(tmp.c_str());
    if (_wfopen_s(&fp, path.c_str(), L"w, ccs=UTF-8") || !fp) return false;
    for (auto& [k, v] : m) fwprintf(fp, L"%ls=%ls\n", k.c_str(), v.c_str());
    fclose(fp);
    return true;
}

}  // namespace ajarq
