// QuickTime Player (Mac): o vídeo numa janela escura do tamanho dele e, por cima, a barra de controles flutuante
// do QuickTime (volume, voltar 15 s, tocar/pausar, avançar 15 s, tempo, barra de progresso e tela cheia), que
// aparece com o mouse e some sozinha. O vídeo é do Media Foundation (MFPlay), que toca o que o Windows toca.
// Espaço tocar/pausar; ← → 5 s; ↑ ↓ volume; Ctrl+F ou duplo clique tela cheia; Esc sai; Ctrl+O abrir; soltar arquivo.
// Uma janela por vídeo, como no Mac.
#include "../comum/app2d.h"
#include <mfplay.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propvarutil.h>
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

using namespace app2d;

namespace {

const UINT WM_MFEVENT = WM_APP + 5;
HWND g_video, g_bar;
IMFPMediaPlayer* g_mp;
std::wstring g_path;
bool g_full = false, g_ended = false, g_ready = false, g_noVideo = false;  // noVideo: só som (sem o codec da imagem)
WINDOWPLACEMENT g_place{ sizeof(g_place) };
float g_volume = 1.0f;
DWORD g_lastMove = 0;
bool g_barShown = false, g_scrubbing = false;
IDWriteTextFormat *g_tfTime, *g_tfIcon, *g_tfIconBig, *g_tfTitle;
struct BarHit { int id; D2D1_RECT_F r; };
std::vector<BarHit> g_hits;
enum { B_VOL = 1, B_BACK, B_PLAY, B_FWD, B_FULL, B_SCRUB };
const float BAR_W = 520, BAR_H = 76;

// ---- MFPlay
class Callback : public IMFPMediaPlayerCallback {
    LONG ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override {
        if (!e) return;
        if (e->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) PostMessageW(wnd, WM_MFEVENT, 1, FAILED(e->hrEvent));
        else if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) PostMessageW(wnd, WM_MFEVENT, 2, 0);
        else if (e->eEventType == MFP_EVENT_TYPE_ERROR) PostMessageW(wnd, WM_MFEVENT, 3, e->hrEvent);
    }
};
Callback* g_cb;

double Pos() {
    if (!g_mp) return 0;
    PROPVARIANT v; PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(g_mp->GetPosition(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_I8) s = v.hVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}
double Len() {
    if (!g_mp) return 0;
    PROPVARIANT v; PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(g_mp->GetDuration(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_UI8) s = v.uhVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}
void Seek(double s) {
    if (!g_mp) return;
    double len = Len();
    s = std::clamp(s, 0.0, len > 0 ? len - 0.05 : s);
    PROPVARIANT v; PropVariantInit(&v); v.vt = VT_I8; v.hVal.QuadPart = (LONGLONG)(s * 1e7);
    g_mp->SetPosition(MFP_POSITIONTYPE_100NS, &v);
    g_ended = false;
}
bool Playing() { MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY; if (g_mp) g_mp->GetState(&st); return st == MFP_MEDIAPLAYER_STATE_PLAYING; }
void Toggle() {
    if (!g_mp) return;
    if (Playing()) g_mp->Pause();
    else { if (g_ended) { Seek(0); } g_mp->Play(); }
}
std::wstring Hms(double s) {
    int t = (int)std::max(0.0, s);
    wchar_t b[24];
    if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); else swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}

// ---- a barra flutuante (janela em camadas, alfa por pixel, por cima do vídeo)
void PaintBar() {
    if (!g_bar) return;
    float s = GetDpiForWindow(wnd) / 96.f;
    RECT cr; GetClientRect(wnd, &cr);
    POINT org{ 0, 0 }; ClientToScreen(wnd, &org);
    int w = (int)std::lround(std::min(BAR_W, cr.right / s - 40) * s), h = (int)std::lround(BAR_H * s);
    if (w < 200) { ShowWindow(g_bar, SW_HIDE); return; }
    int x = org.x + (cr.right - w) / 2, y = org.y + cr.bottom - h - (int)std::lround(24 * s);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt2 = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * s, 96 * s);
    g_hits.clear();
    if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &rt2))) {
        RECT br2{ 0, 0, w, h }; rt2->BindDC(mem, &br2);
        rt2->BeginDraw(); rt2->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* b = nullptr; rt2->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.78f), &b);
        float W = w / s, H = h / s;
        rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 14, 14), b);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.14f)); rt2->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 14, 14), b, 1);
        auto txt = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float a) { b->SetColor(D2D1::ColorF(1, 1, 1, a)); rt2->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, b); };
        // linha de cima: volume à esquerda, ⏪ ⏯ ⏩ no meio, tela cheia à direita
        float cy = 24, cx = W / 2;
        wchar_t vg[2] = { g_volume <= 0.01f ? (wchar_t)0xE74F : (wchar_t)0xE767, 0 };
        txt(vg, g_tfIcon, D2D1::RectF(10, cy - 12, 34, cy + 12), 0.85f);
        D2D1_RECT_F vol = D2D1::RectF(38, cy - 2, 108, cy + 2);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.25f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(vol, 2, 2), b);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(vol.left, vol.top, vol.left + (vol.right - vol.left) * g_volume, vol.bottom), 2, 2), b);
        g_hits.push_back({ B_VOL, D2D1::RectF(vol.left - 4, cy - 10, vol.right + 4, cy + 10) });
        wchar_t back[2] = { 0xED3C, 0 }, fwd[2] = { 0xED3D, 0 }, play[2] = { Playing() ? (wchar_t)0xE769 : (wchar_t)0xE768, 0 }, full[2] = { g_full ? (wchar_t)0xE73F : (wchar_t)0xE740, 0 };
        D2D1_RECT_F rb = D2D1::RectF(cx - 70, cy - 14, cx - 38, cy + 14), rp = D2D1::RectF(cx - 18, cy - 16, cx + 18, cy + 16), rf = D2D1::RectF(cx + 38, cy - 14, cx + 70, cy + 14), rfull = D2D1::RectF(W - 40, cy - 12, W - 12, cy + 12);
        txt(back, g_tfIcon, rb, 0.9f); txt(play, g_tfIconBig, rp, 1); txt(fwd, g_tfIcon, rf, 0.9f); txt(full, g_tfIcon, rfull, 0.85f);
        g_hits.push_back({ B_BACK, rb }); g_hits.push_back({ B_PLAY, rp }); g_hits.push_back({ B_FWD, rf }); g_hits.push_back({ B_FULL, rfull });
        // linha de baixo: tempo, barra, tempo que falta
        double pos = Pos(), len = Len();
        float by = 56;
        txt(Hms(pos), g_tfTime, D2D1::RectF(10, by - 9, 60, by + 9), 0.8f);
        txt(L"-" + Hms(len - pos), g_tfTime, D2D1::RectF(W - 64, by - 9, W - 10, by + 9), 0.8f);
        D2D1_RECT_F bar = D2D1::RectF(64, by - 2, W - 68, by + 2);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.25f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(bar, 2, 2), b);
        float f = len > 0 ? (float)std::clamp(pos / len, 0.0, 1.0) : 0;
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * f, bar.bottom), 2, 2), b);
        g_hits.push_back({ B_SCRUB, D2D1::RectF(bar.left, by - 10, bar.right, by + 10) });
        b->Release(); rt2->EndDraw(); rt2->Release();
    }
    POINT dst{ x, y }, src{ 0, 0 }; SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_bar, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
void ShowBar(bool on) {
    if (!g_bar) return;
    if (on && g_ready) { PaintBar(); if (!g_barShown) { SetWindowPos(g_bar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); g_barShown = true; } }
    else if (!on && g_barShown) { ShowWindow(g_bar, SW_HIDE); g_barShown = false; }
}
void Poke() { g_lastMove = GetTickCount(); ShowBar(true); }

void ScrubTo(float x) {
    for (auto& h : g_hits) if (h.id == B_SCRUB) { double f = std::clamp((x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f); Seek(f * Len()); }
}
void BarAction(int id, float x) {
    switch (id) {
    case B_VOL: for (auto& h : g_hits) if (h.id == B_VOL) { g_volume = std::clamp((x - h.r.left - 4) / (h.r.right - h.r.left - 8), 0.f, 1.f); if (g_mp) g_mp->SetVolume(g_volume); } break;
    case B_BACK: Seek(Pos() - 15); break;
    case B_FWD: Seek(Pos() + 15); break;
    case B_PLAY: Toggle(); break;
    case B_FULL: PostMessageW(wnd, WM_KEYDOWN, 'F', 1); break;
    case B_SCRUB: g_scrubbing = true; SetCapture(g_bar); ScrubTo(x); break;
    }
    PaintBar();
}
LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float s = GetDpiForWindow(h) / 96.f;
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: g_lastMove = GetTickCount(); if (g_scrubbing) { ScrubTo(GET_X_LPARAM(l) / s); PaintBar(); } return 0;
    case WM_LBUTTONDOWN: { float x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s; for (auto& ht : g_hits) if (In(ht.r, x, y)) { BarAction(ht.id, x); break; } return 0; }
    case WM_LBUTTONUP: if (g_scrubbing) { g_scrubbing = false; ReleaseCapture(); } return 0;
    case WM_LBUTTONDBLCLK: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- janela
void FitWindowToVideo() {
    if (!g_mp || g_full) return;
    SIZE vs{}, ar{};
    if (FAILED(g_mp->GetNativeVideoSize(&vs, &ar)) || !vs.cx || !vs.cy) return;
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    float maxW = (mi.rcWork.right - mi.rcWork.left) * 0.72f, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 0.78f;
    float k = std::min({ 1.f, maxW / vs.cx, maxH / vs.cy });
    RECT r{ 0, 0, (LONG)(vs.cx * k), (LONG)(vs.cy * k) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(wnd));
    int W = r.right - r.left, H = r.bottom - r.top;
    SetWindowPos(wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, SWP_NOZORDER);
}

void Open(const std::wstring& path) {
    if (g_mp) { g_mp->Shutdown(); g_mp->Release(); g_mp = nullptr; }
    g_path = path; g_ready = false; g_ended = false;
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    SetWindowTextW(wnd, name.c_str());
    if (!g_cb) g_cb = new Callback;
    HRESULT hr = MFPCreateMediaPlayer(path.c_str(), TRUE, 0, g_cb, g_video, &g_mp);
    if (FAILED(hr)) { MessageBoxW(wnd, Tr(L"Não foi possível abrir este vídeo."), L"QuickTime Player", MB_OK | MB_ICONERROR); return; }
    g_mp->SetVolume(g_volume);
    SHAddToRecentDocs(SHARD_PATHW, path.c_str());
}

void OpenDialog() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Filmes", L"*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.wmv;*.webm;*.mp3;*.m4a;*.wav" }, { L"Todos", L"*.*" } };
    d->SetFileTypes(2, f);
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        if (g_path.empty()) Open(p); else { wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); ShellExecuteW(nullptr, L"open", me, (L"\"" + std::wstring(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
        CoTaskMemFree(p); } it->Release(); } }
    d->Release();
}

void SetFull(bool on) {
    if (on == g_full) return;
    LONG st = GetWindowLongW(wnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(wnd, &g_place);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &g_place);
        SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_full = on;
    Poke();
}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g_mp && g_ready && !g_noVideo) g_mp->UpdateVideo();
        else {
            FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (g_noVideo) {  // tocando só o som: o Windows não tem o codec da imagem (HEVC, ProRes...)
                RECT r; GetClientRect(h, &r); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 200));
                NONCLIENTMETRICSW nm{ sizeof(nm) }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0); HFONT fo = CreateFontIndirectW(&nm.lfMessageFont); HGDIOBJ old = SelectObject(dc, fo);
                DrawTextW(dc, Tr(L"Só o som deste vídeo pode ser tocado: o Windows não tem o codec da imagem (por exemplo HEVC). Instale as Extensões de Vídeo HEVC da Microsoft Store."), -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
                SelectObject(dc, old); DeleteObject(fo);
            }
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: { static LPARAM last; if (l != last) { last = l; Poke(); } return 0; }
    case WM_LBUTTONDOWN: SetFocus(wnd); return 0;
    case WM_LBUTTONDBLCLK: SetFull(!g_full); return 0;
    case WM_DROPFILES: return SendMessageW(wnd, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: if (g_video) { SetWindowPos(g_video, nullptr, 0, 0, LOWORD(l), HIWORD(l), SWP_NOZORDER); if (g_mp) g_mp->UpdateVideo(); } if (g_barShown) PaintBar(); return 0;
    case WM_MOVE: if (g_barShown) PaintBar(); return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_MFEVENT:
        if (w == 1) {
            if (l) { MessageBoxW(h, Tr(L"Não foi possível abrir este vídeo."), L"QuickTime Player", MB_OK | MB_ICONERROR); return 0; }
            g_ready = true;
            { SIZE vs{}, ar{}; g_noVideo = !g_mp || FAILED(g_mp->GetNativeVideoSize(&vs, &ar)) || !vs.cx; }
            FitWindowToVideo(); if (g_mp) g_mp->Play(); Poke(); InvalidateRect(g_video, nullptr, TRUE);
        } else if (w == 2) { g_ended = true; ShowBar(true); }
        return 0;
    case WM_TIMER:  // a barra some depois de 2,5 s sem mexer o mouse (tocando); o tempo anda
        if (g_barShown) {
            POINT p; GetCursorPos(&p); RECT br; GetWindowRect(g_bar, &br);
            bool over = PtInRect(&br, p) != FALSE;
            if (Playing() && !over && !g_scrubbing && GetTickCount() - g_lastMove > 2500) ShowBar(false); else PaintBar();
        }
        if (g_full && !g_barShown) { static DWORD t; if (GetTickCount() - g_lastMove > 2500 && GetTickCount() - t > 1000) { t = GetTickCount(); SetCursor(nullptr); } }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) { POINT p; GetCursorPos(&p); RECT br{}; if (g_bar) GetWindowRect(g_bar, &br); if (!PtInRect(&br, p)) ShowBar(false); } else Poke(); return 0;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (w == VK_SPACE || w == 'K') Toggle();
        else if (w == VK_LEFT) Seek(Pos() - 5);
        else if (w == VK_RIGHT) Seek(Pos() + 5);
        else if (w == 'J') Seek(Pos() - 10);
        else if (w == 'L') Seek(Pos() + 10);
        else if (w == VK_UP) { g_volume = std::min(1.f, g_volume + 0.1f); if (g_mp) g_mp->SetVolume(g_volume); }
        else if (w == VK_DOWN) { g_volume = std::max(0.f, g_volume - 0.1f); if (g_mp) g_mp->SetVolume(g_volume); }
        else if ((ctrl && w == 'F') || (w == 'F' && l == 1)) SetFull(!g_full);
        else if (w == VK_ESCAPE && g_full) SetFull(false);
        else if (ctrl && w == 'O') OpenDialog();
        else if (ctrl && w == 'W') DestroyWindow(h);
        else if (w == VK_HOME) Seek(0);
        Poke();
        return 0;
    }
    case WM_DROPFILES: { wchar_t p[MAX_PATH * 2] = {}; if (DragQueryFileW((HDROP)w, 0, p, MAX_PATH * 2)) Open(p); DragFinish((HDROP)w); return 0; }
    case WM_DESTROY:
        if (g_mp) { g_mp->Shutdown(); g_mp->Release(); g_mp = nullptr; }
        if (g_bar) DestroyWindow(g_bar);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    MFStartup(MF_VERSION);
    g_tfTime = Fmt(11, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfIconBig = Fmt(24, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    WNDCLASSEXW wc{ sizeof(wc) }; wc.style = CS_DBLCLKS; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"QuickTimeJanela";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    WNDCLASSEXW vc{ sizeof(vc) }; vc.style = CS_DBLCLKS; vc.lpfnWndProc = VideoProc; vc.hInstance = hi; vc.lpszClassName = L"QuickTimeVideo"; vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&vc);
    WNDCLASSEXW bc{ sizeof(bc) }; bc.style = CS_DBLCLKS; bc.lpfnWndProc = BarProc; bc.hInstance = hi; bc.lpszClassName = L"QuickTimeBarra"; bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&bc);
    MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c); GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float s = GetDpiForSystem() / 96.f; int W = (int)(800 * s), H = (int)(480 * s);
    wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"QuickTime Player", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    if (!wnd) return 1;
    BOOL dark = TRUE; DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark));  // o QuickTime é sempre escuro
    g_video = CreateWindowExW(WS_EX_ACCEPTFILES, vc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, W, H, wnd, nullptr, hi, nullptr);
    g_bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, wnd, nullptr, hi, nullptr);
    SetTimer(wnd, 1, 250, nullptr);
    ShowWindow(wnd, show);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && argv[1][0]) Open(argv[1]); else PostMessageW(wnd, WM_KEYDOWN, 'O', 0x1D0001);
    if (argv) LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == wnd && msg.wParam == 'O' && msg.lParam == 0x1D0001) { OpenDialog(); continue; }  // sem arquivo: pergunta qual
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    MFShutdown();
    return 0;
}
