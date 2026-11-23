// Siri com a chave do próprio usuário (issue #39, 23/11/2026): o WacOS não tem servidor; quem usa cola a chave de um
// provedor de IA ou usa um modelo local, e as perguntas vão direto do PC para ele.
// Usado pelos Ajustes (escolher o provedor, colar/apagar a chave, atualizar a lista de modelos) e pelo Shell
// (src/shell/siri.cpp: o painel). Aqui fica o miolo: HTTP (WinHTTP), o JSON mínimo, a lista de modelos e as respostas.
// Ajustes em ajustes.txt: siri_provedor (índice em Provedores(); 0 = desligada), siri_modelo, siri_modelos_<n> (a lista
// que o serviço deu na última vez, separada por |). Os índices nunca mudam de lugar: só se acrescenta no fim.
// A chave fica em %APPDATA%\Shell\siri-chave.bin, cifrada com o DPAPI do Windows (só esta conta deste PC abre); nunca
// no ajustes.txt nem em log.
// Os modelos mudam todo mês (o gemini-2.5-flash saiu do ar para contas novas em 2026): a lista vem do próprio serviço
// (ListaModelos), e uma pergunta para um modelo que não existe mais troca sozinha para um que existe (siri.cpp).
#pragma once
#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace siri {

enum Formato { F_NADA, F_GEMINI, F_OPENAI, F_ANTHROPIC, F_OLLAMA, F_CLI };

struct Provedor {
    const wchar_t* nome;
    Formato fmt;
    const wchar_t* host;          // servidor (vazio nos CLIs)
    const wchar_t* caminho;       // caminho da pergunta (Gemini: {m} = modelo); nos CLIs, o comando
    const wchar_t* caminhoModelos;  // caminho da lista de modelos (vazio: sem lista)
    unsigned short porta;         // 443 = https
    bool precisaChave;
    const wchar_t* preferido;     // palavra do modelo para escolher na lista (o mais barato e rápido que responde bem)
    std::vector<const wchar_t*> modelos;  // sugestões antes de buscar a lista; o primeiro é o padrão
    const wchar_t* site;          // onde pegar a chave
};

inline const std::vector<Provedor>& Provedores() {
    static const std::vector<Provedor> v = {
        { L"Desligada", F_NADA, L"", L"", L"", 0, false, L"", {}, L"" },
        { L"Google Gemini", F_GEMINI, L"generativelanguage.googleapis.com", L"/v1beta/models/{m}:generateContent", L"/v1beta/models?pageSize=200", 443, true,
          L"gemini-flash-latest", { L"gemini-flash-latest", L"gemini-3.8-flash", L"gemini-flash-lite-latest", L"gemini-pro-latest" }, L"aistudio.google.com" },
        { L"Groq", F_OPENAI, L"api.groq.com", L"/openai/v1/chat/completions", L"/openai/v1/models", 443, true,
          L"gpt-oss-120b", { L"openai/gpt-oss-120b", L"llama-3.3-70b-versatile" }, L"console.groq.com" },
        { L"OpenAI", F_OPENAI, L"api.openai.com", L"/v1/chat/completions", L"/v1/models", 443, true,
          L"mini", { L"gpt-5-mini", L"gpt-5.5", L"gpt-5-nano" }, L"platform.openai.com" },
        { L"Ollama (local)", F_OLLAMA, L"localhost", L"/api/chat", L"/api/tags", 11434, false,
          L"", { L"llama3.2", L"qwen3", L"gemma3" }, L"ollama.com" },
        { L"Anthropic Claude", F_ANTHROPIC, L"api.anthropic.com", L"/v1/messages", L"/v1/models", 443, true,
          L"haiku", { L"claude-haiku-5-5", L"claude-sonnet-5-5", L"claude-opus-5-5" }, L"console.anthropic.com" },
        { L"OpenRouter", F_OPENAI, L"openrouter.ai", L"/api/v1/chat/completions", L"/api/v1/models", 443, true,
          L"openrouter/auto", { L"openrouter/auto" }, L"openrouter.ai" },
        { L"Mistral", F_OPENAI, L"api.mistral.ai", L"/v1/chat/completions", L"/v1/models", 443, true,
          L"small-latest", { L"mistral-small-latest", L"mistral-large-latest" }, L"console.mistral.ai" },
        { L"DeepSeek", F_OPENAI, L"api.deepseek.com", L"/chat/completions", L"/models", 443, true,
          L"deepseek-chat", { L"deepseek-chat", L"deepseek-reasoner" }, L"platform.deepseek.com" },
        { L"xAI Grok", F_OPENAI, L"api.x.ai", L"/v1/chat/completions", L"/v1/models", 443, true,
          L"mini", { L"grok-4", L"grok-3-mini" }, L"console.x.ai" },
        { L"LM Studio (local)", F_OPENAI, L"localhost", L"/v1/chat/completions", L"/v1/models", 1234, false,
          L"", {}, L"lmstudio.ai" },
        // CLIs instalados no PC: usam a conta que já está entrada neles (sem chave aqui). A pergunta vai pela entrada padrão.
        { L"Codex (local)", F_CLI, L"", L"codex exec --skip-git-repo-check --sandbox read-only --color never --output-last-message ultima.txt -", L"", 0, false,
          L"", {}, L"developers.openai.com/codex" },
        { L"Claude Code (local)", F_CLI, L"", L"claude -p", L"", 0, false,
          L"", {}, L"claude.com/claude-code" },
    };
    return v;
}

// ---------------------------------------------------------------- chave (DPAPI)
// uma chave por serviço: siri-chave-<n>.bin (o Gemini, o primeiro, continua no siri-chave.bin da build-34)
inline std::wstring ArquivoChave(int prov) {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    if (d.empty()) return L"";
    return prov == 1 ? d + L"\\siri-chave.bin" : d + L"\\siri-chave-" + std::to_wstring(prov) + L".bin";
}

inline bool GravaChave(int prov, const std::wstring& chave) {
    std::wstring f = ArquivoChave(prov);
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

inline std::wstring LeChave(int prov) {
    std::wstring f = ArquivoChave(prov), r;
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

// ---------------------------------------------------------------- texto e JSON (o mínimo)
inline std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}
inline std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n);
    return o;
}
inline std::string JStr(const std::wstring& s) {  // texto entre aspas do JSON
    std::string u = Utf8(s), o = "\"";
    for (unsigned char c : u) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
// o texto do primeiro "chave": "..." depois de `de` (desfaz os escapes, \uXXXX com pares substitutos)
inline bool JLe(const std::string& s, const char* chave, size_t de, std::wstring& out, size_t* fim = nullptr) {
    std::string k = std::string("\"") + chave + "\"";
    for (size_t i = s.find(k, de == std::string::npos ? 0 : de); i != std::string::npos; i = s.find(k, i + 1)) {
        size_t j = i + k.size();
        while (j < s.size() && isspace((unsigned char)s[j])) j++;
        if (j >= s.size() || s[j] != ':') continue;
        j++;
        while (j < s.size() && isspace((unsigned char)s[j])) j++;
        if (j >= s.size() || s[j] != '"') continue;
        std::wstring w; std::string run;
        auto flush = [&] { if (!run.empty()) { w += Wide(run); run.clear(); } };
        for (j++; j < s.size() && s[j] != '"'; j++) {
            if (s[j] != '\\') { run += s[j]; continue; }
            if (++j >= s.size()) break;
            char c = s[j];
            if (c == 'n') run += '\n'; else if (c == 't') run += '\t'; else if (c == 'r') {}
            else if (c == 'u' && j + 4 < s.size()) {
                flush();
                w += (wchar_t)strtoul(s.substr(j + 1, 4).c_str(), nullptr, 16);  // UTF-16: o par substituto se monta sozinho
                j += 4;
            } else run += c;
        }
        flush();
        out = w;
        if (fim) *fim = j;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- HTTP
struct Resposta { DWORD status = 0, falha = 0; std::string corpo; };
inline Resposta Http(const Provedor& pv, const wchar_t* metodo, const std::wstring& caminho, const std::wstring& cabecalhos, const std::string& corpo, DWORD esperaMs = 60000) {
    Resposta r;
    HINTERNET s = WinHttpOpen(L"WacOS-Siri/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, pv.host, pv.porta, 0) : nullptr;
    HINTERNET q = c ? WinHttpOpenRequest(c, metodo, caminho.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, pv.porta == 443 ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, (int)esperaMs);
    if (q && WinHttpSendRequest(q, cabecalhos.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : cabecalhos.c_str(), cabecalhos.empty() ? 0 : (DWORD)-1L,
                                corpo.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)corpo.data(), (DWORD)corpo.size(), (DWORD)corpo.size(), 0) &&
        WinHttpReceiveResponse(q, nullptr)) {
        DWORD sz = sizeof(r.status);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (DWORD n = 0; WinHttpQueryDataAvailable(q, &n) && n && r.corpo.size() < 8 * 1024 * 1024;) {
            std::string b(n, 0); DWORD lido = 0;
            if (!WinHttpReadData(q, b.data(), n, &lido) || !lido) break;
            r.corpo.append(b.data(), lido);
        }
    } else r.falha = GetLastError();
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return r;
}
// cabeçalhos de autorização de cada formato (a chave some da memória junto com eles, por quem chamou)
inline std::wstring Cabecalhos(const Provedor& pv, const std::wstring& chave) {
    std::wstring h = L"Content-Type: application/json\r\n";
    if (chave.empty()) return h;
    if (pv.fmt == F_GEMINI) h += L"x-goog-api-key: " + chave + L"\r\n";
    else if (pv.fmt == F_ANTHROPIC) h += L"x-api-key: " + chave + L"\r\nanthropic-version: 2023-06-01\r\n";
    else h += L"Authorization: Bearer " + chave + L"\r\n";
    return h;
}
// a mensagem de erro que o serviço mandou (os formatos: {"error":{"message":...}} ou {"error":"..."})
inline std::wstring ErroDoServico(const std::string& corpo) {
    std::wstring t;
    size_t e = corpo.find("\"error\"");
    if (e != std::string::npos && (JLe(corpo, "message", e, t) || JLe(corpo, "error", e, t))) return t;
    return L"";
}

// ---------------------------------------------------------------- lista de modelos
// Os modelos de conversa que o serviço tem agora (sem os de imagem, voz, embeddings...). Vazio + erro: não deu.
inline std::vector<std::wstring> ListaModelos(int p, std::wstring& erro) {
    std::vector<std::wstring> out;
    const auto& provs = Provedores();
    if (p <= 0 || p >= (int)provs.size()) return out;
    const auto& pv = provs[p];
    if (!*pv.caminhoModelos) return out;
    std::wstring chave = pv.precisaChave ? LeChave(p) : L"";
    if (pv.precisaChave && chave.empty()) { erro = L"sem chave"; return out; }
    std::wstring cab = Cabecalhos(pv, chave);
    SecureZeroMemory(chave.data(), chave.size() * sizeof(wchar_t));
    Resposta r = Http(pv, L"GET", pv.caminhoModelos, cab, "", 20000);
    SecureZeroMemory(cab.data(), cab.size() * sizeof(wchar_t));
    if (r.falha) { erro = L"sem conexão"; return out; }
    if (r.status < 200 || r.status >= 300) { erro = ErroDoServico(r.corpo); if (erro.empty()) erro = L"HTTP " + std::to_wstring(r.status); return out; }
    const char* campo = pv.fmt == F_GEMINI || pv.fmt == F_OLLAMA ? "name" : "id";
    for (size_t at = 0;;) {
        std::wstring v; size_t fim = 0;
        if (!JLe(r.corpo, campo, at, v, &fim)) break;
        at = fim + 1;
        if (pv.fmt == F_GEMINI) {
            if (v.rfind(L"models/", 0) != 0) continue;  // o "name" de outras coisas
            v = v.substr(7);
        }
        std::wstring l = v; for (auto& ch : l) ch = (wchar_t)towlower(ch);
        bool fora = false;
        for (const wchar_t* w : { L"embed", L"whisper", L"tts", L"dall-e", L"image", L"audio", L"realtime", L"moderation", L"transcribe",
                                  L"search", L"aqa", L"imagen", L"veo", L"guard", L"vision-", L"computer-use", L"native-audio", L"live", L"robotics", L"banana", L"customtools" })
            if (l.find(w) != std::wstring::npos) { fora = true; break; }
        if (pv.fmt == F_GEMINI && l.find(L"gemini") == std::wstring::npos && l.find(L"gemma") == std::wstring::npos) fora = true;
        if (p == 3 && !(l.rfind(L"gpt", 0) == 0 || l.rfind(L"o", 0) == 0 || l.rfind(L"chatgpt", 0) == 0)) fora = true;  // OpenAI: só os de conversa
        if (!fora && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        if (out.size() >= 400) break;
    }
    // o preferido primeiro, depois em ordem; os mais novos costumam ter número maior: ordem decrescente
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return a > b; });
    if (*pv.preferido) {
        // o nome exato primeiro (os apelidos que o serviço mantém atualizados, como gemini-flash-latest); senão, o primeiro
        // que tem a palavra
        auto it = std::find(out.begin(), out.end(), std::wstring(pv.preferido));
        if (it == out.end()) it = std::find_if(out.begin(), out.end(), [&](const std::wstring& m) {
            std::wstring l = m; for (auto& ch : l) ch = (wchar_t)towlower(ch);
            return l.find(pv.preferido) != std::wstring::npos && l.find(L"lite") == std::wstring::npos && l.find(L"preview") == std::wstring::npos &&
                   l.find(L"exp") == std::wstring::npos && l.find(L"thinking") == std::wstring::npos;
        });
        if (it != out.end()) std::rotate(out.begin(), it, it + 1);
    }
    return out;
}

// ---------------------------------------------------------------- CLI local (Codex, Claude Code)
// Roda o comando com a pergunta pela entrada padrão, numa pasta temporária, sem janela; devolve a saída (UTF-8).
// falha: o comando não existe (9009 do cmd) ou passou do tempo.
// cancelado (opcional): consultada enquanto espera; true encerra o CLI (pergunta descartada: outra feita ou painel fechado).
// O cmd e o que ele abre (o node do Codex/Claude Code) ficam num job: encerrar só o cmd deixava o node rodando escondido
// até terminar (gastando a cota da conta), e várias perguntas seguidas empilhavam processos.
inline std::wstring RodaCli(const std::wstring& comando, const std::wstring& pergunta, DWORD& codigo, bool& tempo,
                            bool (*cancelado)(void*) = nullptr, void* ctx = nullptr) {
    codigo = 0; tempo = false;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        for (HANDLE x : { inR, inW, outR, outW, job }) if (x) CloseHandle(x);
        return L"";
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"WacOS-Siri";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring cmd = L"cmd.exe /d /c " + comando;
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);  // os avisos (stderr) não entram na resposta
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    // suspenso até entrar no job (o cmd não abre o node antes disso)
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi);
    if (!ok) codigo = GetLastError();
    CloseHandle(inR); CloseHandle(outW); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); if (job) CloseHandle(job); return L""; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    std::string u = Utf8(pergunta); DWORD n = 0;
    WriteFile(inW, u.data(), (DWORD)u.size(), &n, nullptr);
    CloseHandle(inW);  // fim da entrada: o CLI começa
    std::string saida; char b[4096]; DWORD lido = 0;
    ULONGLONG ate = GetTickCount64() + 180000;
    for (;;) {
        DWORD disp = 0;
        if (!PeekNamedPipe(outR, nullptr, 0, nullptr, &disp, nullptr)) break;  // o processo fechou a saída
        if (disp) { if (!ReadFile(outR, b, sizeof(b), &lido, nullptr) || !lido) break; saida.append(b, lido); continue; }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(outR, nullptr, 0, nullptr, &disp, nullptr) && disp && ReadFile(outR, b, sizeof(b), &lido, nullptr) && lido) saida.append(b, lido);
            break;
        }
        if (GetTickCount64() > ate) { tempo = true; break; }
        if (cancelado && cancelado(ctx)) { codigo = ERROR_CANCELLED; break; }
    }
    if (tempo || codigo == ERROR_CANCELLED) { if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1); }
    else GetExitCodeProcess(pi.hProcess, &codigo);
    CloseHandle(outR); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (job) CloseHandle(job);  // KILL_ON_JOB_CLOSE: o que ainda sobrou do CLI sai junto
    return Wide(saida);
}

}  // namespace siri
