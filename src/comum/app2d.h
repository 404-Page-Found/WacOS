#pragma once
// Base dos apps pequenos desenhados com Direct2D (Lembretes, Calendário): fábricas, fonte SF/Inter/Segoe,
// tema claro/escuro, texto, campo de edição (EDIT do Windows por cima, sem borda) e o arquivo de dados em
// %APPDATA%\Shell. A barra de título do Mac vem da WacOSWindow.dll (a janela é comum, com barra do Windows).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#include "idioma.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace app2d {

inline ID2D1Factory* d2d;
inline IDWriteFactory* dw;
inline std::wstring family = L"Segoe UI";
inline HWND wnd;
inline ID2D1HwndRenderTarget* rt;
inline ID2D1SolidColorBrush* br;

inline bool HasFont(const wchar_t* name) {
    IDWriteFontCollection* c = nullptr; dw->GetSystemFontCollection(&c);
    UINT32 i; BOOL ex = FALSE;
    if (c) { c->FindFamilyName(name, &i, &ex); c->Release(); }
    return ex != FALSE;
}

inline bool Init() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
    if (!d2d || !dw) return false;
    family = HasFont(L"SF Pro Text") ? L"SF Pro Text" : HasFont(L"Inter") ? L"Inter" : L"Segoe UI";
    return true;
}

inline bool Dark() {
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}
inline float Sc() { return GetDpiForWindow(wnd) / 96.f; }

inline IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
                              bool wrap = false, const wchar_t* fam = nullptr) {
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(fam ? fam : family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* e = nullptr; dw->CreateEllipsisTrimmingSign(f, &e);
        f->SetTrimming(&t, e); if (e) e->Release();
    }
    return f;
}
inline const wchar_t* IconFamily() { static const wchar_t* f = HasFont(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets"; return f; }

inline float TextW(IDWriteTextFormat* f, const std::wstring& s) {
    IDWriteTextLayout* l = nullptr;
    dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &l);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); l->Release();
    return m.widthIncludingTrailingWhitespace;
}

inline void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (s.empty() || !f) return;
    br->SetColor(c);
    // CLIP | ENABLE_COLOR_FONT (0x4): emojis em cor (Clima, Fotos...)
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br, (D2D1_DRAW_TEXT_OPTIONS)(D2D1_DRAW_TEXT_OPTIONS_CLIP | 0x4));
}
inline void Fill(D2D1_RECT_F r, D2D1_COLOR_F c, float rad = 0) {
    br->SetColor(c);
    if (rad > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br); else rt->FillRectangle(r, br);
}
inline void Circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col, bool fill, float stroke = 1.5f) {
    br->SetColor(col);
    if (fill) rt->FillEllipse(D2D1::Ellipse(c, r, r), br); else rt->DrawEllipse(D2D1::Ellipse(c, r, r), br, stroke);
}
inline D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
inline bool In(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

inline bool BeginPaint() {
    if (!rt) {
        RECT r; GetClientRect(wnd, &r);
        float dpi = (float)GetDpiForWindow(wnd);
        d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi),
                                    D2D1::HwndRenderTargetProperties(wnd, D2D1::SizeU(r.right, r.bottom)), &rt);
        if (rt) rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    }
    if (!rt) return false;
    rt->BeginDraw();
    return true;
}
inline void EndPaint() {
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { br->Release(); br = nullptr; rt->Release(); rt = nullptr; InvalidateRect(wnd, nullptr, FALSE); }
}
inline void OnSize(LPARAM l) { if (rt) rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); InvalidateRect(wnd, nullptr, FALSE); }
inline void OnDpi(WPARAM w, LPARAM l) {
    RECT* r = (RECT*)l;
    SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (rt) rt->SetDpi((float)HIWORD(w), (float)HIWORD(w));
}
inline D2D1_SIZE_F Size() { RECT r; GetClientRect(wnd, &r); float s = Sc(); return { r.right / s, r.bottom / s }; }

// ---- campo de edição sobre o desenho (EDIT do Windows, sem borda, na fonte do app)
struct Field {
    HWND h = nullptr;
    WNDPROC old = nullptr;
    HFONT font = nullptr;
    std::function<void(const std::wstring&, int key)> done;  // key: VK_RETURN, VK_ESCAPE, VK_TAB ou 0 (perdeu o foco)
    bool ending = false;
};
inline Field field;
inline LRESULT CALLBACK FieldProc(HWND h, UINT m, WPARAM w, LPARAM l);
inline void FieldEnd(int key) {
    if (!field.h || field.ending) return;
    field.ending = true;
    int n = GetWindowTextLengthW(field.h);
    std::wstring t(n, L'\0'); GetWindowTextW(field.h, t.data(), n + 1);
    HWND h = field.h; field.h = nullptr;
    auto done = field.done; field.done = nullptr;
    DestroyWindow(h);
    field.ending = false;
    if (done) done(t, key);
    InvalidateRect(wnd, nullptr, FALSE);
}
inline LRESULT CALLBACK FieldProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) { FieldEnd((int)w); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == L'\t')) return 0;
    if (m == WM_KILLFOCUS) { PostMessageW(wnd, WM_APP + 77, 0, 0); }
    return CallWindowProcW(field.old, h, m, w, l);
}
// r em DIP; size em pt
inline void FieldOpen(D2D1_RECT_F r, const std::wstring& text, float size, bool bold, std::function<void(const std::wstring&, int)> done) {
    FieldEnd(0);
    float s = Sc();
    if (field.font) DeleteObject(field.font);
    field.font = CreateFontW(-(int)std::lround(size * s), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, family.c_str());
    int hgt = (int)std::lround(size * s * 1.45f);
    int y = (int)std::lround((r.top + r.bottom) / 2 * s) - hgt / 2;
    field.h = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, (int)std::lround(r.left * s), y,
                              (int)std::lround((r.right - r.left) * s), hgt, wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!field.h) return;
    field.done = std::move(done);
    SendMessageW(field.h, WM_SETFONT, (WPARAM)field.font, TRUE);
    field.old = (WNDPROC)SetWindowLongPtrW(field.h, GWLP_WNDPROC, (LONG_PTR)FieldProc);
    SendMessageW(field.h, EM_SETSEL, 0, -1);
    SetFocus(field.h);
}
// cores do campo (WM_CTLCOLOREDIT): fundo do app e texto do tema
inline HBRUSH g_fieldBrush;
inline LRESULT FieldColors(HDC dc, COLORREF bg) {
    bool dark = Dark();
    SetTextColor(dc, dark ? RGB(240, 240, 240) : RGB(20, 20, 20));
    SetBkColor(dc, bg);
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(bg);
    return (LRESULT)g_fieldBrush;
}

// ---- dados em %APPDATA%\Shell
inline std::wstring DataPath(const wchar_t* name) {
    wchar_t ad[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    std::wstring d = std::wstring(ad) + L"\\Shell";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\" + name;
}
inline std::vector<std::wstring> ReadLines(const std::wstring& path) {
    std::vector<std::wstring> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return v;
    wchar_t line[8192];
    while (fgetws(line, 8192, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        v.push_back(s);
    }
    fclose(f);
    return v;
}
inline bool WriteLines(const std::wstring& path, const std::vector<std::wstring>& lines) {
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w, ccs=UTF-8") || !f) return false;
    for (auto& l : lines) fwprintf(f, L"%ls\n", l.c_str());
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}
// campos separados por TAB; TAB e quebra de linha dentro do texto viram \t e \n
inline std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'\\') o += L"\\\\"; else if (c == L'\t') o += L"\\t"; else if (c == L'\n') o += L"\\n"; else if (c != L'\r') o += c; }
    return o;
}
inline std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t n = s[++i]; o += n == L't' ? L'\t' : n == L'n' ? L'\n' : n; }
        else o += s[i];
    }
    return o;
}
inline std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> v; size_t a = 0, b;
    while ((b = s.find(L'\t', a)) != std::wstring::npos) { v.push_back(s.substr(a, b - a)); a = b + 1; }
    v.push_back(s.substr(a));
    return v;
}

// ---- datas: minutos desde 1970 (hora local)
inline long long NowMin() { return (long long)time(nullptr) / 60; }
inline long long ToMin(const SYSTEMTIME& st) { tm t{}; t.tm_year = st.wYear - 1900; t.tm_mon = st.wMonth - 1; t.tm_mday = st.wDay; t.tm_hour = st.wHour; t.tm_min = st.wMinute; t.tm_isdst = -1; return (long long)mktime(&t) / 60; }
inline SYSTEMTIME FromMin(long long m) {
    time_t s = (time_t)(m * 60); tm t{}; localtime_s(&t, &s);
    SYSTEMTIME st{}; st.wYear = (WORD)(t.tm_year + 1900); st.wMonth = (WORD)(t.tm_mon + 1); st.wDay = (WORD)t.tm_mday; st.wHour = (WORD)t.tm_hour; st.wMinute = (WORD)t.tm_min; st.wDayOfWeek = (WORD)t.tm_wday;
    return st;
}
inline long long DayStart(long long m) { SYSTEMTIME s = FromMin(m); s.wHour = s.wMinute = 0; return ToMin(s); }
inline const wchar_t* MonthName(int m) {  // 1..12
    static const wchar_t* pt[] = { L"janeiro", L"fevereiro", L"março", L"abril", L"maio", L"junho", L"julho", L"agosto", L"setembro", L"outubro", L"novembro", L"dezembro" };
    static const wchar_t* en[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December" };
    return idioma::En() ? en[(m - 1) % 12] : pt[(m - 1) % 12];
}
inline const wchar_t* WeekdayShort(int d) {  // 0 = domingo
    static const wchar_t* pt[] = { L"dom.", L"seg.", L"ter.", L"qua.", L"qui.", L"sex.", L"sáb." };
    static const wchar_t* en[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    return idioma::En() ? en[d % 7] : pt[d % 7];
}
// "Hoje, 14:30" / "Amanhã" / "12/11/2026, 09:00"
inline std::wstring DueText(long long m, bool withTime) {
    if (!m) return L"";
    long long today = DayStart(NowMin()), day = DayStart(m);
    SYSTEMTIME s = FromMin(m);
    wchar_t b[64];
    std::wstring d;
    if (day == today) d = Tr(L"Hoje");
    else if (day == today + 24 * 60) d = Tr(L"Amanhã");
    else if (day == today - 24 * 60) d = Tr(L"Ontem");
    else { swprintf_s(b, L"%02d/%02d/%04d", s.wDay, s.wMonth, s.wYear); d = b; }
    if (withTime) { swprintf_s(b, L", %02d:%02d", s.wHour, s.wMinute); d += b; }
    return d;
}

inline HWND CreateMain(HINSTANCE hi, const wchar_t* cls, const wchar_t* title, WNDPROC proc, int w, int h) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float s = GetDpiForSystem() / 96.f;
    int W = (int)(w * s), H = (int)(h * s);
    wnd = CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2,
                          mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    if (wnd) { BOOL dark = Dark(); DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark)); }
    return wnd;
}

// ---- barra lateral tingida pelo papel de parede (macOS 26, "Permitir tingir janelas com o papel de parede";
// Ajustes > Aparência > Transparência, tingir_janelas=1 por padrão). Só a "cor" do papel de parede entra (o
// quanto ele foge do cinza), não o claro/escuro: a barra continua clara no tema claro e escura no escuro.
inline bool TintOn() {
    for (auto& l : ReadLines(DataPath(L"ajustes.txt"))) if (l.rfind(L"tingir_janelas=", 0) == 0) return l.substr(15) != L"0";
    return true;
}
inline bool WallpaperAverage(float& r, float& g, float& b) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // (já iniciado: não faz nada)
    wchar_t wp[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0) || !wp[0]) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(wp, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, 24, 24, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> px(24 * 24 * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, 24 * 4, (UINT)px.size(), px.data()))) {
            double R = 0, G = 0, B = 0;
            for (size_t i = 0; i < px.size(); i += 4) { B += px[i]; G += px[i + 1]; R += px[i + 2]; }
            double n = 24 * 24 * 255.0;
            r = (float)(R / n); g = (float)(G / n); b = (float)(B / n);
            ok = true;
        }
    }
    if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return ok;
}
inline D2D1_COLOR_F Tinted(D2D1_COLOR_F base) {
    static int state = -1;  // -1: não lido; 0: sem tinta; 1: com
    static float dr, dg, db;
    if (state < 0) {
        state = 0;
        float r, g, b;
        if (TintOn() && WallpaperAverage(r, g, b)) {
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            auto lim = [](float v) { return std::clamp(v, -0.3f, 0.3f); };
            dr = lim(r - lum); dg = lim(g - lum); db = lim(b - lum);
            state = 1;
        }
    }
    if (state != 1) return base;
    float k = Dark() ? 0.11f : 0.09f;  // leve, como no Mac (mais forte a barra ficava da cor do papel de parede)
    auto cl = [](float v) { return std::clamp(v, 0.f, 1.f); };
    return D2D1::ColorF(cl(base.r + dr * k), cl(base.g + dg * k), cl(base.b + db * k), base.a);
}
inline D2D1_COLOR_F SidebarColor() { return Tinted(Dark() ? D2D1::ColorF(40 / 255.f, 40 / 255.f, 42 / 255.f) : D2D1::ColorF(242 / 255.f, 242 / 255.f, 245 / 255.f)); }

inline int Run() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

}  // namespace app2d
