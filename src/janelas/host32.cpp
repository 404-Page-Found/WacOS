// MacJanela32Host.exe (32 bits): põe o gancho da MacJanela32.dll nas threads de programas de 32 bits
// que o Shell (64 bits) indicar. Um gancho só pode carregar uma DLL da mesma arquitetura.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <string>

static std::map<DWORD, HHOOK> g_hooks;
static HMODULE g_dll;
static HOOKPROC g_proc;
const UINT HOST_HOOK = WM_APP + 1, HOST_QUIT = WM_APP + 2;

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == HOST_HOOK) {
        DWORD tid = (DWORD)w;
        if (g_proc && !g_hooks.count(tid))
            if (HHOOK hk = SetWindowsHookExW(WH_GETMESSAGE, g_proc, g_dll, tid)) g_hooks[tid] = hk;
        if (l) PostMessageW((HWND)l, WM_NULL, 0, 0);
        return 0;
    }
    if (m == HOST_QUIT) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) {
        for (auto& [t, hk] : g_hooks) UnhookWindowsHookEx(hk);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
    // cópia com nome próprio: os apps que já têm uma versão antiga presa carregam esta como outro módulo
    std::wstring copies = dir + L"\\obj\\mj";
    CreateDirectoryW((dir + L"\\obj").c_str(), nullptr);
    CreateDirectoryW(copies.c_str(), nullptr);
    // nome pela data do arquivo (como no Shell): reabrir reaproveita a cópia; cada nome novo ocupa um dos
    // 32 lugares de DLL de gancho do Windows até os programas que a carregaram fecharem
    std::wstring src = dir + L"\\WacOSWindow32.dll", name;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) {
        wchar_t b[40]; swprintf_s(b, L"WacOSWindow32-%08lx%08lx.dll", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
        name = b;
    }
    if (name.empty()) return 1;  // sem cópia própria não põe gancho (mesmo caminho com outra versão trava o programa)
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((copies + L"\\WacOSWindow32-*.dll").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do if (_wcsicmp(fd.cFileName, name.c_str())) DeleteFileW((copies + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(f, &fd));  // as em uso ficam
        FindClose(f);
    }
    std::wstring copy = copies + L"\\" + name;
    if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(src.c_str(), copy.c_str(), TRUE)) return 1;
    g_dll = LoadLibraryW(copy.c_str());
    g_proc = g_dll ? (HOOKPROC)GetProcAddress(g_dll, "GetMsgProc") : nullptr;
    if (!g_proc) return 1;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"MacJanela32Host";
    RegisterClassW(&wc);
    if (!CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr)) return 1;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
