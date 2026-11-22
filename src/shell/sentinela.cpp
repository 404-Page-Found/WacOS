// Sentinela: a thread da interface do Shell parou (issue #32: "o Shell inteiro congela por um bom tempo")?
// Uma thread própria manda WM_NULL para a janela de mensagens a cada 250 ms (SendMessageTimeout: responde
// também dentro de menus e arrastos, que têm laço próprio). Sem resposta em 1 s, copia a pilha da thread da
// interface (suspensa só durante a cópia: GetThreadContext + memcpy, nada que pegue trava) e, já solta, grava no
// shell.log "interface parada" com a mensagem que estava sendo tratada, a janela e a pilha em módulo+deslocamento
// (o deslocamento do WacOSShell.exe se resolve com o WacOSShell.pdb da mesma build). Quando volta, grava quanto
// tempo ficou parada. No máximo 30 registros por execução.
#include "shell.h"
#include <algorithm>
#include <thread>
#include <atomic>

namespace sentinela {
std::atomic<UINT> ultimaMsg{ 0 };
std::atomic<HWND> ultimaJanela{ nullptr };
}

namespace {

HWND g_alvo;
DWORD g_tid;
const size_t COPIA = 96 * 1024;  // o topo da pilha basta para as ~30 chamadas de cima
BYTE g_copia[COPIA];

// pilha da thread em `pcs` (endereços de retorno); devolve quantos
int Pilha(HANDLE t, DWORD64* pcs, int max) {
    CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
    size_t n = 0;
    DWORD64 orig = 0;
    if (SuspendThread(t) == (DWORD)-1) return 0;
    bool ok = GetThreadContext(t, &c) != FALSE;
    if (ok) {
        orig = c.Rsp;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((void*)orig, &mbi, sizeof(mbi))) {
            DWORD64 fim = (DWORD64)mbi.BaseAddress + mbi.RegionSize;
            n = (size_t)std::min<DWORD64>(COPIA, fim - orig);
            memcpy(g_copia, (void*)orig, n);
        }
    }
    ResumeThread(t);
    if (!ok || !n) return 0;
    const DWORD64 delta = (DWORD64)g_copia - orig;
    auto mudar = [&](DWORD64& r) { if (r >= orig && r < orig + n) r += delta; };
    auto dentro = [&](DWORD64 a) { return a >= (DWORD64)g_copia && a + 8 <= (DWORD64)g_copia + n; };
    int k = 0;
    __try {
        mudar(c.Rsp); mudar(c.Rbp);
        while (k < max && c.Rip) {
            pcs[k++] = c.Rip;
            DWORD64 img = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &img, nullptr);
            if (!fe) {  // função folha: o retorno está no topo
                if (!dentro(c.Rsp)) break;
                c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8;
            } else {
                void* hd = nullptr; DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &hd, &ef, nullptr);
            }
            mudar(c.Rsp); mudar(c.Rbp);
            if (!dentro(c.Rsp)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return k;
}

std::wstring Nome(DWORD64 pc) {
    HMODULE m = nullptr;
    wchar_t b[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc, &m) && m)
        GetModuleFileNameW(m, b, MAX_PATH);
    const wchar_t* s = wcsrchr(b, L'\\');
    wchar_t o[MAX_PATH + 24];
    swprintf_s(o, L"%ls+0x%llX", s ? s + 1 : b, m ? (unsigned long long)(pc - (DWORD64)m) : (unsigned long long)pc);
    return o;
}

void Vigiar() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_tid);
    if (!t) { Log(L"sentinela: sem acesso à thread da interface (%lu)", GetLastError()); return; }
    int registros = 0;
    for (;;) {
        Sleep(250);
        DWORD_PTR r = 0;
        DWORD t0 = GetTickCount();
        if (SendMessageTimeoutW(g_alvo, WM_NULL, 0, 0, SMTO_NORMAL, 1000, &r)) continue;
        if (!IsWindow(g_alvo)) return;  // o Shell está saindo
        if (registros >= 30) {  // já registrou bastante: só espera voltar
            while (!SendMessageTimeoutW(g_alvo, WM_NULL, 0, 0, SMTO_NORMAL, 1000, &r) && IsWindow(g_alvo)) {}
            continue;
        }
        UINT msg = sentinela::ultimaMsg; HWND jan = sentinela::ultimaJanela;
        DWORD64 pcs[40];
        int n = Pilha(t, pcs, 40);
        wchar_t cls[64] = L"?";
        if (jan) GetClassNameW(jan, cls, 64);
        std::wstring p;
        for (int i = 0; i < n; i++) { if (i) p += L" < "; p += Nome(pcs[i]); }
        registros++;
        Log(L"sentinela: interface parada há 1 s (mensagem 0x%04X em %ls); pilha: %ls", msg, cls, p.c_str());
        // espera voltar, para dizer quanto durou (com uma segunda pilha se passar de 4 s: mostra se mudou de lugar)
        bool segunda = false;
        while (!SendMessageTimeoutW(g_alvo, WM_NULL, 0, 0, SMTO_NORMAL, 500, &r)) {
            if (!IsWindow(g_alvo)) return;
            if (!segunda && GetTickCount() - t0 > 4000) {
                segunda = true;
                n = Pilha(t, pcs, 40);
                p.clear();
                for (int i = 0; i < n; i++) { if (i) p += L" < "; p += Nome(pcs[i]); }
                Log(L"sentinela: ainda parada (%lu ms); pilha: %ls", GetTickCount() - t0, p.c_str());
            }
        }
        Log(L"sentinela: a interface voltou depois de %lu ms", GetTickCount() - t0);
    }
}

}  // namespace

void sentinela::Start(HWND alvo) {
    g_alvo = alvo;
    g_tid = GetWindowThreadProcessId(alvo, nullptr);
    std::thread(Vigiar).detach();
}
