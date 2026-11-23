// Siri (issue #39, 23/11/2026): pergunta pelo Spotlight ("Perguntar à Siri") e a resposta num painel de vidro no alto
// da tela, como a Siri do macOS. Não há servidor do WacOS: a pergunta vai direto do PC para o serviço que o usuário
// escolheu nos Ajustes (Gemini, Groq, OpenAI ou o Ollama local), com a chave dele (src/comum/siri.h).
// A chamada roda numa thread (WinHTTP, até 60 s); cada pergunta tem um número, e a resposta de uma pergunta antiga
// (painel fechado ou outra pergunta feita) é descartada. Esc ou clicar fora fecha; Ctrl+C copia a resposta.
#include "shell.h"
#include "../comum/siri.h"
#include "../comum/simbolos.h"
#include "../comum/idioma.h"
#include <winhttp.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#pragma comment(lib, "winhttp.lib")

namespace {

const UINT WM_SIRI_FIM = WM_APP + 70, WM_SIRI_FRAME = WM_APP + 71;
const UINT_PTR T_PONTOS = 1;
const float W = 560, PAD = 20, ORB = 30, R = 22;

HWND g_wnd, g_back;
ui::GpuSurface g_s;
bool g_glass, g_inGlass, g_open;
std::wstring g_q, g_a;
bool g_busy, g_erro;
std::atomic<int> g_num{ 0 };
int g_pontos;
float g_scroll, g_contentH, g_viewH;
double g_openAt;
IDWriteTextFormat *g_tfQ, *g_tfA;

// ---------------------------------------------------------------- JSON (o mínimo: montar o pedido e ler um texto)
std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n);
    return o;
}
std::string JStr(const std::wstring& s) {  // texto entre aspas do JSON
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
bool JLe(const std::string& s, const char* chave, size_t de, std::wstring& out, size_t* fim = nullptr) {
    std::string k = std::string("\"") + chave + "\"";
    for (size_t i = s.find(k, de); i != std::string::npos; i = s.find(k, i + 1)) {
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

// ---------------------------------------------------------------- a pergunta (thread)
std::wstring Limpa(std::wstring t) {  // os modelos insistem em Markdown: tira o que fica feio em texto puro
    for (const wchar_t* m : { L"**", L"__", L"`" }) for (size_t i; (i = t.find(m)) != std::wstring::npos;) t.erase(i, wcslen(m));
    std::wstring o; bool ini = true;
    for (size_t i = 0; i < t.size(); i++) {
        if (ini) {  // "# Título" e "* item" no começo da linha
            while (i < t.size() && t[i] == L'#') i++;
            if (i + 1 < t.size() && (t[i] == L'*' || t[i] == L'-') && t[i + 1] == L' ') { o += L"• "; i += 2; }
            while (i < t.size() && t[i] == L' ' && !o.empty() && o.back() == L'\n') i++;
        }
        if (i >= t.size()) break;
        o += t[i];
        ini = t[i] == L'\n';
    }
    while (!o.empty() && iswspace(o.back())) o.pop_back();
    size_t s = 0; while (s < o.size() && iswspace(o[s])) s++;
    return o.substr(s);
}

std::wstring Pergunta(const std::wstring& q, bool& erro) {
    erro = true;
    const auto& provs = siri::Provedores();
    int p = std::clamp(_wtoi(shell::Setting(L"siri_provedor", L"0").c_str()), 0, (int)provs.size() - 1);
    if (p == 0) return Tr(L"Escolha um serviço para a Siri em Ajustes do Sistema > Apple Intelligence e Siri.");
    const auto& pv = provs[p];
    std::wstring modelo = shell::Setting(L"siri_modelo", pv.modelos.empty() ? L"" : pv.modelos[0]);
    std::wstring chave = pv.precisaChave ? siri::LeChave() : L"";
    if (pv.precisaChave && chave.empty()) return Tr(L"Falta a chave da API. Cole a chave em Ajustes do Sistema > Apple Intelligence e Siri.");
    const std::wstring sistema = L"You are Siri inside WacOS, a macOS-style desktop for Windows. Answer briefly and clearly, "
                                 L"in the same language as the question. Use plain text, without Markdown.";
    std::string body;
    std::wstring caminho = pv.caminho, cab = L"Content-Type: application/json\r\n";
    if (p == 1) {  // Gemini
        size_t m = caminho.find(L"{m}"); if (m != std::wstring::npos) caminho.replace(m, 3, modelo);
        body = "{\"systemInstruction\":{\"parts\":[{\"text\":" + JStr(sistema) + "}]},\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":" + JStr(q) + "}]}]}";
        cab += L"x-goog-api-key: " + chave + L"\r\n";
    } else {  // OpenAI, Groq (o mesmo formato) e Ollama
        body = "{\"model\":" + JStr(modelo) + ",\"messages\":[{\"role\":\"system\",\"content\":" + JStr(sistema) + "},{\"role\":\"user\",\"content\":" + JStr(q) + "}]" +
               (p == 4 ? ",\"stream\":false}" : "}");
        if (!chave.empty()) cab += L"Authorization: Bearer " + chave + L"\r\n";
    }
    SecureZeroMemory(chave.data(), chave.size() * sizeof(wchar_t));
    std::string resp; DWORD status = 0, falha = 0;
    HINTERNET s = WinHttpOpen(L"WacOS-Siri/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, pv.host, pv.porta, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", caminho.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, pv.porta == 443 ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    if (r && WinHttpSendRequest(r, cab.c_str(), (DWORD)-1L, body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (DWORD n = 0; WinHttpQueryDataAvailable(r, &n) && n && resp.size() < 4 * 1024 * 1024;) {
            std::string b(n, 0); DWORD lido = 0;
            if (!WinHttpReadData(r, b.data(), n, &lido) || !lido) break;
            resp.append(b.data(), lido);
        }
    } else falha = GetLastError();
    // a chave ia no cabeçalho: some da memória junto com o pedido
    SecureZeroMemory(cab.data(), cab.size() * sizeof(wchar_t));
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    Log(L"siri: %ls %ls -> %lu (%zu bytes, erro %lu)", pv.nome, modelo.c_str(), status, resp.size(), falha);
    if (falha) {
        if (p == 4) return Tr(L"Não deu para falar com o Ollama neste PC. Ele está aberto?");
        return Tr(L"Não deu para falar com o serviço. Confira a internet.");
    }
    std::wstring t;
    if (status >= 200 && status < 300) {
        bool ok = p == 1 ? JLe(resp, "text", resp.find("\"candidates\"") == std::string::npos ? 0 : resp.find("\"candidates\""), t)
                         : JLe(resp, "content", resp.find("\"message\"") == std::string::npos ? 0 : resp.find("\"message\""), t);
        if (ok && !t.empty()) { erro = false; return Limpa(t); }
        return Tr(L"O serviço respondeu sem texto.");
    }
    size_t e = resp.find("\"error\"");
    if (e != std::string::npos && (JLe(resp, "message", e, t) || JLe(resp, "error", e > 0 ? e - 1 : 0, t)) && !t.empty())
        return (status == 401 || status == 403 ? std::wstring(Tr(L"Chave recusada pelo serviço.")) + L" " : std::wstring()) + t;
    return std::wstring(Tr(L"O serviço recusou a pergunta")) + L" (" + std::to_wstring(status) + L").";
}

// ---------------------------------------------------------------- painel
float Sc() { return g_wnd ? ui::Scale(g_wnd) : 1.f; }

float AlturaTexto(const std::wstring& t, IDWriteTextFormat* f, float w) {
    IDWriteTextLayout* l = nullptr; float h = 0;
    if (f && SUCCEEDED(ui::dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, w, 10000, &l))) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); h = m.height; l->Release(); }
    return h;
}

void Orbe(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float r, double t) {
    // a bolinha colorida da Siri: um anel de cores que gira devagar enquanto pensa
    D2D1_GRADIENT_STOP gs[4] = { { 0, D2D1::ColorF(0.98f, 0.36f, 0.62f) }, { 0.35f, D2D1::ColorF(0.62f, 0.38f, 1.f) },
                                 { 0.7f, D2D1::ColorF(0.25f, 0.62f, 1.f) }, { 1, D2D1::ColorF(1.f, 0.62f, 0.32f) } };
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(dc->CreateGradientStopCollection(gs, 4, &sc))) return;
    float a = (float)(g_busy ? t / 900.0 : 0.6) ;
    D2D1_POINT_2F p0{ c.x + std::cos(a) * r, c.y + std::sin(a) * r }, p1{ c.x - std::cos(a) * r, c.y - std::sin(a) * r };
    ID2D1LinearGradientBrush* lg = nullptr;
    if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), sc, &lg))) {
        dc->FillEllipse(D2D1::Ellipse(c, r, r), lg);
        lg->Release();
    }
    sc->Release();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.35f), &br);
    if (br) { dc->DrawEllipse(D2D1::Ellipse(c, r - 0.5f, r - 0.5f), br, 1); br->Release(); }
}

void Paint() {
    if (!g_open || !g_wnd) return;
    float sc = Sc();
    bool dark = ui::Dark();
    float textW = W - PAD * 2 - ORB - 14;
    std::wstring a = g_busy ? std::wstring(Tr(L"Pensando")) + std::wstring((size_t)(g_pontos % 3 + 1), L'.') : g_a;
    float qh = AlturaTexto(g_q, g_tfQ, textW), ah = AlturaTexto(a, g_tfA, textW);
    g_contentH = PAD + qh + 8 + ah + PAD;
    HMONITOR hm = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    float maxH = (mi.rcWork.bottom - mi.rcWork.top) / sc * 0.6f;
    float H = std::clamp(g_contentH, ORB + PAD * 2, maxH);
    g_viewH = H;
    g_scroll = std::clamp(g_scroll, 0.f, std::max(0.f, g_contentH - H));
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    RECT wr; GetWindowRect(g_wnd, &wr);
    if (wr.bottom - wr.top != hpx || wr.right - wr.left != wpx) {
        SetWindowPos(g_wnd, nullptr, 0, 0, wpx, hpx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_back) SetWindowPos(g_back, g_wnd, wr.left, wr.top, wpx, hpx, SWP_NOACTIVATE);
    }
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                     dark ? D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.55f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.50f), 24.f);
    ID2D1DeviceContext* dc = g_glass ? backdrop::ContentBegin(g_back, wpx, hpx, sc) : nullptr;
    g_inGlass = dc != nullptr;
    if (!dc) dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (br) {
        D2D1_RECT_F box = D2D1::RectF(0, 0, W, H);
        if (!g_glass) { br->SetColor(dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.95f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.95f)); dc->FillRoundedRectangle(D2D1::RoundedRect(box, R, R), br); }
        if (liquid::On()) liquid::Rim(dc, box, R, dark);
        else { br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br, 1); }
        Orbe(dc, { PAD + ORB / 2, PAD + ORB / 2 }, ORB / 2, anim::Now() - g_openAt);
        float x = PAD + ORB + 14;
        dc->PushAxisAlignedClip(D2D1::RectF(x - 2, 8, W - PAD + 2, H - 8), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = PAD - g_scroll;
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f));
        dc->DrawTextW(g_q.c_str(), (UINT32)g_q.size(), g_tfQ, D2D1::RectF(x, y, x + textW, y + qh + 4), br);
        y += qh + 8;
        br->SetColor(g_erro ? D2D1::ColorF(1.f, 0.42f, 0.38f) : dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.88f));
        dc->DrawTextW(a.c_str(), (UINT32)a.size(), g_tfA, D2D1::RectF(x, y, x + textW, y + ah + 4), br);
        dc->PopAxisAlignedClip();
        if (g_contentH > H) {  // barrinha de rolagem fina
            float bh = std::max(24.f, H * H / g_contentH), by = 6 + (H - 12 - bh) * (g_scroll / (g_contentH - H));
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.3f) : D2D1::ColorF(0, 0, 0, 0.25f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(W - 8, by, W - 4, by + bh), 2, 2), br);
        }
        br->Release();
    }
    if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s);
}

void Fecha() {
    if (!g_open) return;
    g_open = false; g_busy = false;
    g_num++;  // a resposta que ainda vem é descartada
    KillTimer(g_wnd, T_PONTOS);
    ui::FrameClock(g_wnd, WM_SIRI_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    if (g_back) ShowWindow(g_back, SW_HIDE);
}

void Copia() {
    if (g_busy || g_a.empty() || !OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    size_t n = (g_a.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(g), g_a.c_str(), n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIRI_FIM: {
        std::wstring* r = (std::wstring*)l;
        if ((int)w == g_num && g_open) {
            g_erro = r->size() > 1 && (*r)[0] == L'\x1';
            g_a = g_erro ? r->substr(1) : *r;
            g_busy = false; g_scroll = 0;
            KillTimer(h, T_PONTOS);
            ui::FrameClock(h, WM_SIRI_FRAME, false);
            Paint();
        }
        delete r;
        return 0;
    }
    case WM_SIRI_FRAME: ui::FrameAck(h, WM_SIRI_FRAME); Paint(); return 0;
    case WM_TIMER: if (w == T_PONTOS) { g_pontos++; Paint(); } return 0;
    case WM_MOUSEWHEEL: g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * 48; Paint(); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Fecha();
        else if (w == 'C' && GetKeyState(VK_CONTROL) < 0) Copia();
        else if (w == VK_DOWN) { g_scroll += 40; Paint(); }
        else if (w == VK_UP) { g_scroll -= 40; Paint(); }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) Fecha(); return 0;
    case WM_CLOSE: Fecha(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Ready() {
    if (g_wnd && IsWindow(g_wnd)) return true;
    HINSTANCE hi = shell::inst;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellSiri"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellSiriFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Siri", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) { Log(L"siri: janela não criada (%lu)", GetLastError()); return false; }
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) { DestroyWindow(g_back); g_back = nullptr; }
    apps::ours.push_back(g_wnd); if (g_back) apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_tfQ = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfA = ui::Format(15, DWRITE_FONT_WEIGHT_NORMAL);
    for (auto* f : { g_tfQ, g_tfA }) if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return true;
}

}  // namespace

bool siri::Pronta() {
    const auto& provs = siri::Provedores();
    int p = _wtoi(shell::Setting(L"siri_provedor", L"0").c_str());
    if (p <= 0 || p >= (int)provs.size()) return false;
    return !provs[p].precisaChave || GetFileAttributesW(siri::ArquivoChave().c_str()) != INVALID_FILE_ATTRIBUTES;
}

void siri::Perguntar(const std::wstring& q) {
    if (q.empty() || !Ready()) return;
    g_q = q; g_a.clear(); g_busy = true; g_erro = false; g_scroll = 0; g_pontos = 0;
    int num = ++g_num;
    // no alto do monitor do ponteiro, centralizado (onde a Siri do Mac aparece... e onde o Spotlight estava)
    POINT pt; GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    float sc = ui::Scale(g_wnd);
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround((ORB + PAD * 2) * sc);
    int x = (mi.rcWork.left + mi.rcWork.right - wpx) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 6;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    if (g_back) SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE);
    g_open = true; g_openAt = anim::Now();
    Paint();
    if (g_back) ShowWindow(g_back, SW_SHOWNOACTIVATE);
    ShowWindow(g_wnd, SW_SHOW);
    shell::LiberaFrente();
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    SetTimer(g_wnd, T_PONTOS, 420, nullptr);
    ui::FrameClock(g_wnd, WM_SIRI_FRAME, true);  // a bolinha gira enquanto pensa
    HWND to = g_wnd;
    std::thread([q, num, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool erro = false;
        std::wstring r = Pergunta(q, erro);
        PostMessageW(to, WM_SIRI_FIM, (WPARAM)num, (LPARAM)new std::wstring((erro ? L"\x1" : L"") + r));
        CoUninitialize();
    }).detach();
}
