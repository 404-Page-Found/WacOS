// Brilho da tela na Ilha (issue #39, 23/11/2026): as teclas de brilho do notebook não chegam como teclas (o firmware
// muda o brilho direto e o Windows mostra o aviso dele), então o volume tinha aviso na Ilha e o brilho não.
// Uma thread espera o evento do WMI (root\WMI, WmiMonitorBrightnessEvent: o mesmo que o Windows usa) e manda o valor
// para a Ilha (ilha::Brilho). Só existe em tela embutida de notebook; num PC de mesa a consulta falha e a thread sai.
// O aviso do próprio Windows continua aparecendo: ele é do Explorador e não tem como ser desligado por fora.
#include "shell.h"
#include <wbemidl.h>
#include <thread>
#include <algorithm>
#pragma comment(lib, "wbemuuid.lib")

void brilho::Start() {
    std::thread([] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        IWbemLocator* loc = nullptr; IWbemServices* sv = nullptr; IEnumWbemClassObject* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc)))) {
            BSTR ns = SysAllocString(L"ROOT\\WMI");
            if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &sv)) && sv) {
                CoSetProxyBlanket(sv, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessEvent");
                HRESULT hr = sv->ExecNotificationQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
                SysFreeString(lang); SysFreeString(q);
                if (FAILED(hr)) { en = nullptr; Log(L"brilho: sem tela com brilho pelo WMI (0x%08X); a Ilha não mostra o brilho", (unsigned)hr); }
                else Log(L"brilho: vigiando o brilho da tela (WMI)");
            }
            SysFreeString(ns);
        }
        while (en) {
            IWbemClassObject* o = nullptr; ULONG n = 0;
            HRESULT hr = en->Next(WBEM_INFINITE, 1, &o, &n);
            if (FAILED(hr)) { Log(L"brilho: a vigia parou (0x%08X)", (unsigned)hr); break; }
            if (!n || !o) continue;
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(o->Get(L"Brightness", 0, &v, nullptr, nullptr)) && SUCCEEDED(VariantChangeType(&v, &v, 0, VT_I4)))
                ilha::Brilho(std::clamp((int)v.lVal, 0, 100));
            VariantClear(&v); o->Release();
        }
        if (en) en->Release();
        if (sv) sv->Release();
        if (loc) loc->Release();
        CoUninitialize();
    }).detach();
}
