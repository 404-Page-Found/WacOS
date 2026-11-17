// Quick Look do Shell, como no Mac: espaço num arquivo da mesa (ou da pilha de Downloads, ou ⌘Y/Ctrl+Y no
// Spotlight) abre um painel só com a prévia do arquivo; espaço ou Esc fecha. A prévia é a miniatura grande do
// Windows (fotos, PDFs, vídeos e documentos com visualizador instalado) ou o texto, nos arquivos de texto.
// Barra de cima: fechar à esquerda, nome no meio, "Abrir com <app>" à direita. Arrasta pela barra.
#include "shell.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <cmath>
#include <thread>
#include "../comum/idioma.h"

namespace {

const UINT WM_QL_READY = WM_APP + 91;
const float BAR = 38, R = 12, PAD = 0;
HWND g_wnd;
ui::Surface g_s;
std::wstring g_path, g_name, g_app, g_text;
UINT g_iw, g_ih; std::vector<BYTE> g_px;
ID2D1Bitmap* g_bmp;
ID2D1RenderTarget* g_bmpRt;
int g_gen;
bool g_loading, g_hoverClose, g_hoverOpen;
IDWriteTextFormat *g_tfTitle, *g_tfBtn, *g_tfText;
D2D1_RECT_F g_closeR, g_openR;

float Sc() { return ui::Scale(g_wnd); }

bool IsTextFile(const std::wstring& p) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    for (auto e : { L".txt", L".md", L".log", L".csv", L".json", L".xml", L".ini", L".cfg", L".yml", L".yaml", L".cpp", L".h", L".c",
                    L".hpp", L".py", L".js", L".ts", L".ps1", L".cmd", L".bat", L".html", L".htm", L".css", L".reg", L".inc" })
        if (x == e) return true;
    return false;
}

std::wstring ReadText(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::vector<char> b(64 * 1024);
    DWORD n = 0; ReadFile(f, b.data(), (DWORD)b.size(), &n, nullptr);
    CloseHandle(f);
    b.resize(n);
    if (n >= 2 && (BYTE)b[0] == 0xFF && (BYTE)b[1] == 0xFE) return std::wstring((wchar_t*)(b.data() + 2), (n - 2) / 2);
    int off = n >= 3 && (BYTE)b[0] == 0xEF && (BYTE)b[1] == 0xBB && (BYTE)b[2] == 0xBF ? 3 : 0;
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data() + off, (int)n - off, nullptr, 0);
    UINT cp = len > 0 ? CP_UTF8 : CP_ACP;
    len = MultiByteToWideChar(cp, 0, b.data() + off, (int)n - off, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, 0, b.data() + off, (int)n - off, w.data(), len);
    return w;
}

// os apps do Mac do projeto para o tipo (como no Mac: Pré-Visualização abre imagens e PDF, TextEdit o texto)
std::wstring g_openExe;
std::wstring OwnApp(const std::wstring& p, std::wstring& name) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    const wchar_t* exe = nullptr;
    for (auto e : { L".pdf", L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic", L".ico" }) if (x == e) { exe = EXE_PREVIEW; name = Tr(L"Pré-Visualização"); }
    for (auto e : { L".txt", L".rtf", L".md", L".log" }) if (x == e) { exe = EXE_TEXTEDIT; name = L"TextEdit"; }
    for (auto e : { L".mp3", L".m4a", L".flac", L".wav", L".aac", L".ogg", L".wma", L".aif", L".aiff", L".opus" }) if (x == e) { exe = EXE_MUSIC; name = Tr(L"Música"); }
    for (auto e : { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm" }) if (x == e) { exe = EXE_QUICKTIME; name = L"QuickTime Player"; }
    if (!exe) return L"";
    std::wstring full = shell::dir + L"\\" + exe;
    return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES ? full : L"";
}
void OpenPath(const std::wstring& p) {
    if (!g_openExe.empty()) ShellExecuteW(nullptr, L"open", g_openExe.c_str(), (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else if (!shell::OpenCalculator(p)) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring DefaultAppName(const std::wstring& p) {
    std::wstring own;
    g_openExe = OwnApp(p, own);
    if (!g_openExe.empty()) return own;
    std::wstring x = PathFindExtensionW(p.c_str());
    wchar_t name[256] = {}; DWORD n = 256;
    if (!x.empty() && SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, x.c_str(), nullptr, name, &n)) && name[0]) return name;
    return L"";
}

// tamanho do painel (DIP) para a prévia: proporção da imagem, até 62% da área de trabalho
SIZE PanelSize(float sc) {
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float W = (mi.rcWork.right - mi.rcWork.left) / sc * 0.62f, H = (mi.rcWork.bottom - mi.rcWork.top) / sc * 0.72f - BAR;
    float w = 560, h = 420;
    if (!g_text.empty()) { w = std::min(W, 720.f); h = std::min(H, 760.f); }
    else if (g_iw && g_ih) {
        float k = std::min(W / g_iw, H / g_ih);
        k = std::min(k, 1.6f / sc);  // imagem pequena não estoura
        w = std::max(360.f, g_iw * k); h = std::max(240.f, g_ih * k);
    }
    return { (LONG)std::lround(w), (LONG)std::lround(h + BAR) };
}

void Paint() {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return;
    float sc = Sc();
    RECT wr; GetWindowRect(g_wnd, &wr);
    int wpx = wr.right - wr.left, hpx = wr.bottom - wr.top;
    auto* rt = ui::Begin(g_s, wpx, hpx, sc);
    if (!rt) return;
    float W = wpx / sc, H = hpx / sc;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    // corpo e barra
    br->SetColor(dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.97f) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.98f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.16f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br, 1);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.08f));
    rt->DrawLine({ 0, BAR }, { W, BAR }, br, 1);
    // fechar (círculo com x, como o do Quick Look)
    g_closeR = D2D1::RectF(12, BAR / 2 - 11, 34, BAR / 2 + 11);
    D2D1_POINT_2F cc{ 23, BAR / 2 };
    br->SetColor(g_hoverClose ? (dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.12f)) : D2D1::ColorF(0, 0, 0, 0));
    rt->FillEllipse(D2D1::Ellipse(cc, 10, 10), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.75f) : D2D1::ColorF(0, 0, 0, 0.65f));
    rt->DrawLine({ cc.x - 4, cc.y - 4 }, { cc.x + 4, cc.y + 4 }, br, 1.4f);
    rt->DrawLine({ cc.x - 4, cc.y + 4 }, { cc.x + 4, cc.y - 4 }, br, 1.4f);
    // "Abrir com ..." à direita
    std::wstring btn = g_app.empty() ? std::wstring(Tr(L"Abrir")) : std::wstring(Tr(L"Abrir com ")) + g_app;
    float bw = ui::TextWidth(g_tfBtn, btn) + 22;
    g_openR = D2D1::RectF(W - 12 - bw, BAR / 2 - 12, W - 12, BAR / 2 + 12);
    br->SetColor(g_hoverOpen ? (dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.12f)) : (dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.06f)));
    rt->FillRoundedRectangle(D2D1::RoundedRect(g_openR, 6, 6), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.85f));
    rt->DrawTextW(btn.c_str(), (UINT32)btn.size(), g_tfBtn, g_openR, br);
    // nome no meio (entre os botões)
    float side = std::max(44.f, bw + 24);
    rt->DrawTextW(g_name.c_str(), (UINT32)g_name.size(), g_tfTitle, D2D1::RectF(side, 0, W - side, BAR), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    // prévia
    D2D1_RECT_F body = D2D1::RectF(PAD, BAR, W - PAD, H - PAD);
    rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
    if (!g_text.empty()) {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.88f) : D2D1::ColorF(0, 0, 0, 0.88f));
        rt->DrawTextW(g_text.c_str(), (UINT32)std::min<size_t>(g_text.size(), 20000), g_tfText, D2D1::RectF(body.left + 18, body.top + 14, body.right - 18, body.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        if (g_bmpRt != rt && g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
        if (!g_bmp && !g_px.empty()) { g_bmp = ui::Bitmap(rt, g_iw, g_ih, g_px); g_bmpRt = rt; }
        if (g_bmp) {
            float bwid = body.right - body.left, bh = body.bottom - body.top;
            float k = std::min(bwid / g_iw, bh / g_ih), w = g_iw * k, h = g_ih * k;
            float x = body.left + (bwid - w) / 2, y = body.top + (bh - h) / 2;
            rt->DrawBitmap(g_bmp, D2D1::RectF(x, y, x + w, y + h), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f));
            const wchar_t* t = g_loading ? Tr(L"Carregando…") : Tr(L"Sem prévia");
            rt->DrawTextW(t, (UINT32)wcslen(t), g_tfTitle, D2D1::RectF(body.left, (body.top + body.bottom) / 2 - 10, body.right, (body.top + body.bottom) / 2 + 10), br);
        }
    }
    rt->PopAxisAlignedClip();
    br->Release();
    ui::End(g_s, true, POINT{ wr.left, wr.top });
}

void Place() {
    float sc = Sc();
    SIZE s = PanelSize(sc);
    int w = (int)std::lround(s.cx * sc), h = (int)std::lround(s.cy * sc);
    RECT wr; GetWindowRect(g_wnd, &wr);
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    if (!IsWindowVisible(g_wnd)) {  // primeira vez: no meio do monitor do mouse
        MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c);
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
        cx = (mi.rcWork.left + mi.rcWork.right) / 2; cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    }
    SetWindowPos(g_wnd, HWND_TOP, cx - w / 2, cy - h / 2, w, h, SWP_NOACTIVATE);
}

void Load(const std::wstring& path) {
    g_path = path;
    g_name = path.substr(path.find_last_of(L"\\/") + 1);
    g_app = DefaultAppName(path);
    g_text.clear(); g_px.clear(); g_iw = g_ih = 0;
    if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
    int gen = ++g_gen;
    if (IsTextFile(path)) { g_text = ReadText(path); g_loading = false; return; }
    g_loading = true;
    HWND w = g_wnd;
    int px = (int)std::lround(1200 * Sc());
    std::thread([path, gen, w, px] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* r = new std::tuple<int, UINT, UINT, std::vector<BYTE>>(gen, 0, 0, std::vector<BYTE>{});
        auto& [g, iw, ih, px2] = *r;
        if (!ui::ShellImage(path, px, iw, ih, px2, false)) ui::ShellImage(path, 256, iw, ih, px2, true);
        if (SUCCEEDED(co)) CoUninitialize();
        if (!PostMessageW(w, WM_QL_READY, 0, (LPARAM)r)) delete r;
    }).detach();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_QL_READY: {
        auto* r = (std::tuple<int, UINT, UINT, std::vector<BYTE>>*)l;
        if (std::get<0>(*r) == g_gen) {
            g_iw = std::get<1>(*r); g_ih = std::get<2>(*r); g_px = std::move(std::get<3>(*r)); g_loading = false;
            if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
            Place(); Paint();
        }
        delete r;
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        float sc = Sc(), x = p.x / sc, y = p.y / sc;
        if (y < BAR && !(x >= g_closeR.left && x < g_closeR.right) && !(x >= g_openR.left && x < g_openR.right)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        float sc = Sc(), x = (short)LOWORD(l) / sc, y = (short)HIWORD(l) / sc;
        bool hc = x >= g_closeR.left && x < g_closeR.right && y >= g_closeR.top && y < g_closeR.bottom;
        bool ho = x >= g_openR.left && x < g_openR.right && y >= g_openR.top && y < g_openR.bottom;
        if (hc != g_hoverClose || ho != g_hoverOpen) { g_hoverClose = hc; g_hoverOpen = ho; Paint(); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hoverClose || g_hoverOpen) { g_hoverClose = g_hoverOpen = false; Paint(); } return 0;
    case WM_LBUTTONUP:
        if (g_hoverClose) { quicklook::Close(); return 0; }
        if (g_hoverOpen) {
            std::wstring p = g_path; quicklook::Close();
            OpenPath(p);
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_SPACE) { quicklook::Close(); return 0; }
        if (w == VK_RETURN) { std::wstring p = g_path; quicklook::Close(); OpenPath(p); return 0; }
        return 0;
    case WM_MOVE: Paint(); return 0;
    case WM_DPICHANGED: Place(); Paint(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool Ensure() {
    if (g_wnd) return true;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellQuickLook"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Quick Look", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    g_s.hwnd = g_wnd;
    apps::ours.push_back(g_wnd);
    g_tfTitle = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBtn = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfText = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    for (auto* f : { g_tfTitle, g_tfBtn }) if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_tfTitle) { DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; g_tfTitle->SetTrimming(&t, nullptr); g_tfTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    return true;
}

}  // namespace

bool quicklook::IsOpen() { return g_wnd && IsWindowVisible(g_wnd); }
std::wstring quicklook::Path() { return IsOpen() ? g_path : L""; }

void quicklook::Close() {
    if (!g_wnd) return;
    ShowWindow(g_wnd, SW_HIDE);
    g_gen++;
    if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
    g_px.clear(); g_text.clear();
}

void quicklook::Toggle(const std::wstring& path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (IsOpen() && !_wcsicmp(path.c_str(), g_path.c_str())) { Close(); return; }
    if (!Ensure()) return;
    Load(path);
    Place();  // fechado: no meio do monitor do mouse; aberto: fica onde está, no tamanho da prévia nova
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    Paint();
    Log(L"quick look: %ls", path.c_str());
}
