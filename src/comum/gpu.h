// Qual placa de vídeo usar para desenhar o que vai para a composição da tela (DirectComposition / Windows.UI.Composition).
//
// D3D11CreateDevice(nullptr, ...) usa o "adaptador padrão", que num notebook com GPU híbrida (Ryzen/Intel + RTX) pode ser a
// placa dedicada: quando o Windows ou o painel da NVIDIA/AMD marca o programa como "alto desempenho", o adaptador 0 vira a
// GPU dedicada, mas quem compõe a tela (o DWM) e liga o monitor do notebook é a integrada. Aí cada quadro do Dock, da barra
// de menus, do vidro e de cada janela capturada atravessa as duas placas (cópia entre adaptadores pelo PCIe) e a GPU
// dedicada fica acordada o tempo todo: ponteiro e janelas engasgando (issue #33, Acer Nitro AN515-45, Ryzen + RTX 3060).
//
// Aqui o adaptador é o que liga o monitor principal (o mesmo do DWM), achado pelo nome do monitor no GDI
// (D3DKMTOpenAdapterFromGdiDisplayName -> LUID -> IDXGIFactory4::EnumAdapterByLuid). Num PC de mesa com o monitor na placa
// dedicada continua a dedicada; com a integrada ligada e sem monitor nela, nada muda. Se algo falhar: nullptr (o padrão).
#pragma once
#include <windows.h>
#include <dxgi1_4.h>

namespace gpu {

// O adaptador da tela principal (com AddRef; quem recebe chama Release) ou nullptr para o padrão do Windows.
inline IDXGIAdapter* AdaptadorDaTela() {
    struct AbrirPorNome { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
    struct Fechar { UINT hAdapter; };
    using FnAbrir = LONG(APIENTRY*)(AbrirPorNome*);
    using FnFechar = LONG(APIENTRY*)(Fechar*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    auto abrir = gdi ? (FnAbrir)GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
    auto fechar = gdi ? (FnFechar)GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr;
    if (!abrir) return nullptr;
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) return nullptr;
    AbrirPorNome a{};
    lstrcpynW(a.DeviceName, mi.szDevice, 32);
    if (abrir(&a) != 0) return nullptr;  // STATUS_SUCCESS = 0
    if (fechar) { Fechar f{ a.hAdapter }; fechar(&f); }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    using FnFab = HRESULT(WINAPI*)(REFIID, void**);
    auto fab = dxgi ? (FnFab)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!fab) return nullptr;
    IDXGIFactory4* f4 = nullptr;
    if (FAILED(fab(__uuidof(IDXGIFactory4), (void**)&f4)) || !f4) return nullptr;
    IDXGIAdapter* ad = nullptr;
    if (FAILED(f4->EnumAdapterByLuid(a.AdapterLuid, __uuidof(IDXGIAdapter), (void**)&ad))) ad = nullptr;
    f4->Release();
    return ad;
}

// Igual ao D3D11CreateDevice(nullptr, HARDWARE, ...), mas na placa da tela (cai para o padrão se não der).
template <class FnCriar>
inline HRESULT CriarNaTela(FnCriar criar, UINT flags, ID3D11Device** dev, ID3D11DeviceContext** ctx, bool* naTela = nullptr) {
    if (naTela) *naTela = false;
    if (IDXGIAdapter* ad = AdaptadorDaTela()) {
        HRESULT hr = criar(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, dev, nullptr, ctx);
        ad->Release();
        if (SUCCEEDED(hr)) { if (naTela) *naTela = true; return hr; }
    }
    return criar(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, dev, nullptr, ctx);
}

// Nome do adaptador do dispositivo (para o log)
inline void NomeDoAdaptador(IUnknown* dev, wchar_t* out, size_t n) {
    out[0] = 0;
    IDXGIDevice* dx = nullptr;
    if (!dev || FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) || !dx) return;
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(ad->GetDesc(&d))) lstrcpynW(out, d.Description, (int)n);
        ad->Release();
    }
    dx->Release();
}

}  // namespace gpu
