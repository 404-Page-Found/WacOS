// WacOSGancho.dll / WacOSGancho32.dll: a DLL que o Windows põe nos apps pelo gancho (SetWindowsHookEx). Só carrega a
// WacOSWindow.dll da versão atual (caminho no mapa do Shell, ver src\comum\gancho_mapa.h) e passa as mensagens para ela.
// Fica igual entre as versões do WacOS: ocupa UM lugar na tabela de DLLs de gancho do Windows, para sempre, e trocar
// a WacOSWindow.dll não gasta lugar nenhum (LoadLibrary). NÃO MUDE ESTE ARQUIVO À TOA: cada binário diferente da
// carregadora ocupa outro lugar (o Shell nomeia a cópia pelo conteúdo, e a compilação é determinística, /Brepro).
#include <windows.h>
#include "../comum/gancho_mapa.h"

#ifndef _WIN64
#pragma comment(linker, "/EXPORT:GetMsgProc=_GetMsgProc@12")
#endif

namespace {
GanchoMapa* volatile g_mapa;
LONG g_gen;                 // o gen do mapa que já foi aplicado
HOOKPROC volatile g_proc;   // GetMsgProc da WacOSWindow.dll atual
SRWLOCK g_trava = SRWLOCK_INIT;

GanchoMapa* Mapa() {
    if (g_mapa) return g_mapa;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, GANCHO_MAPA);
    if (!h) return nullptr;
    void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(GanchoMapa));
    CloseHandle(h);  // a vista mantém o mapa vivo
    if (!v) return nullptr;
    if (((GanchoMapa*)v)->tamanho != sizeof(GanchoMapa)) { UnmapViewOfFile(v); return nullptr; }
    if (InterlockedCompareExchangePointer((PVOID volatile*)&g_mapa, v, nullptr) != nullptr) UnmapViewOfFile(v);
    return g_mapa;
}

void Atualiza(GanchoMapa* m) {
    AcquireSRWLockExclusive(&g_trava);
    LONG gen = m->gen;
    if (gen != g_gen) {
        wchar_t p[520];
#ifdef _WIN64
        lstrcpynW(p, m->real64, 520);
#else
        lstrcpynW(p, m->real32, 520);
#endif
        // a WacOSWindow.dll recusa carregar dentro de jogos (DllMain FALSE): aí não há proc e o gancho só passa adiante
        HMODULE dll = p[0] ? LoadLibraryW(p) : nullptr;
        g_proc = dll ? (HOOKPROC)GetProcAddress(dll, "GetMsgProc") : nullptr;
        g_gen = gen;
    }
    ReleaseSRWLockExclusive(&g_trava);
}
}  // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int code, WPARAM w, LPARAM l) {
    if (GanchoMapa* m = Mapa()) {
        if (m->gen != g_gen) Atualiza(m);
        if (HOOKPROC p = g_proc) return p(code, w, l);  // ela mesma chama o próximo gancho
    }
    return CallNextHookEx(nullptr, code, w, l);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
