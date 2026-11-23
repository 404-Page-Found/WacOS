// Siri com a chave do próprio usuário (issue #39, 23/11/2026): o WacOS não tem servidor; quem usa cola a chave de um
// provedor de IA (Google Gemini, Groq, OpenAI) ou usa um modelo local (Ollama) e as perguntas vão direto do PC para ele.
// Usado pelos Ajustes (escolher o provedor, colar/apagar a chave) e pelo Shell (src/shell/siri.cpp: a pergunta).
// Ajustes em ajustes.txt: siri_provedor (vazio/0 = desligada, 1 Gemini, 2 Groq, 3 OpenAI, 4 Ollama), siri_modelo.
// A chave fica em %APPDATA%\Shell\siri-chave.bin, cifrada com o DPAPI do Windows (só esta conta deste PC abre); nunca
// no ajustes.txt nem em log.
#pragma once
#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#pragma comment(lib, "crypt32.lib")

namespace siri {

struct Provedor {
    const wchar_t* nome;
    const wchar_t* host;      // servidor
    const wchar_t* caminho;   // caminho da chamada (Gemini: {m} = modelo)
    unsigned short porta;     // 443 = https
    bool precisaChave;
    std::vector<const wchar_t*> modelos;  // o primeiro é o padrão
};

inline const std::vector<Provedor>& Provedores() {
    static const std::vector<Provedor> v = {
        { L"Desligada", L"", L"", 0, false, {} },
        { L"Google Gemini", L"generativelanguage.googleapis.com", L"/v1beta/models/{m}:generateContent", 443, true,
          { L"gemini-2.5-flash", L"gemini-2.5-flash-lite", L"gemini-2.5-pro" } },
        { L"Groq", L"api.groq.com", L"/openai/v1/chat/completions", 443, true,
          { L"llama-3.3-70b-versatile", L"llama-3.1-8b-instant", L"openai/gpt-oss-120b" } },
        { L"OpenAI", L"api.openai.com", L"/v1/chat/completions", 443, true,
          { L"gpt-4o-mini", L"gpt-4.1-mini", L"gpt-4.1" } },
        { L"Ollama (local)", L"localhost", L"/api/chat", 11434, false,
          { L"llama3.2", L"qwen2.5", L"gemma3" } },
    };
    return v;
}

inline std::wstring ArquivoChave() {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    return d.empty() ? L"" : d + L"\\siri-chave.bin";
}

inline bool GravaChave(const std::wstring& chave) {
    std::wstring f = ArquivoChave();
    if (f.empty()) return false;
    if (chave.empty()) { DeleteFileW(f.c_str()); return true; }
    CreateDirectoryW(f.substr(0, f.find_last_of(L'\\')).c_str(), nullptr);
    DATA_BLOB in{ (DWORD)(chave.size() * sizeof(wchar_t)), (BYTE*)chave.data() }, out{};
    if (!CryptProtectData(&in, L"WacOS Siri", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    bool ok = false;
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD n = 0; ok = WriteFile(h, out.pbData, out.cbData, &n, nullptr) && n == out.cbData; CloseHandle(h); }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
}

inline std::wstring LeChave() {
    std::wstring f = ArquivoChave(), r;
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    DWORD sz = GetFileSize(h, nullptr), n = 0;
    std::vector<BYTE> b(sz > 0 && sz < 65536 ? sz : 0);
    bool lido = !b.empty() && ReadFile(h, b.data(), sz, &n, nullptr) && n == sz;
    CloseHandle(h);
    if (!lido) return r;
    DATA_BLOB in{ sz, b.data() }, out{};
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        r.assign((wchar_t*)out.pbData, out.cbData / sizeof(wchar_t));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return r;
}

// a chave como aparece nos Ajustes: só os 4 últimos caracteres
inline std::wstring ChaveMascarada(const std::wstring& k) {
    if (k.empty()) return L"";
    return L"••••" + (k.size() > 4 ? k.substr(k.size() - 4) : L"");
}

}  // namespace siri
