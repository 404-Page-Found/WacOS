// "Sacudir o ponteiro para localizar" do macOS: sacudir o mouse rápido faz o ponteiro crescer (até 6×) e,
// parando, ele volta ao normal.
// Detecção (a mesma ideia do efeito do KDE, que é aberto; a Apple não publica a dela): guarda as posições
// do último ~0,7 s, junta os trechos retos num só e compara o caminho percorrido com a diagonal da área
// coberta. Sacudir dá um caminho muito maior que a área (> 4×); arrastar ou ir de um canto a outro, não.
// O mouse é lido por "raw input" numa thread só dela: sem gancho, o ponteiro nunca fica mais lento.
// Crescer: o cursor do sistema que está na tela (seta, texto ou mãozinha) é trocado por ele mesmo em
// tamanho maior, tirado do próprio arquivo do esquema (os do Mac têm desenhos até 128 px: nítido até 4×).
// Voltar: o esquema é recarregado do registro (SPI_SETCURSORS, sem gravar nada). Se o Shell sair ou cair,
// o cursor volta (Stop aqui e o vigia da mesa).
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <thread>
#include <atomic>

namespace {

const double WINDOW_MS = 700;     // histórico de posições
const float SENSITIVITY = 4.f;    // caminho / diagonal para contar como sacudida
const float MIN_DIAG = 90.f;      // px (a 100%): sacudida pequena demais não conta
const float FIRST = 4.0f, STEP = 0.75f, MAXMAG = 6.0f;  // 4× na primeira, +0,75 a cada sacudida seguida (o Mac cresce bem)
// Voltar ao normal como no Mac: ele não espera — assim que o mouse desacelera, encolhe. Antes ficava 700 ms grande
// depois da última sacudida e encolhia em 260 ms (o usuário: "no macOS ele some muito mais rápido").
const double SLOW_MS = 110;       // sem movimento rápido por isto (as viradas da sacudida são mais curtas): encolhe
const double HOLD_MS = 450;       // ainda andando rápido mas sem sacudir (atravessando a tela): encolhe também
const float FAST = 1.1f;          // px/ms (a 100%): acima disso é "movimento rápido"
const double GROW_MS = 200, SHRINK_MS = 170;

struct Sample { double t; float x, y; };
std::deque<Sample> g_hist;
std::atomic<DWORD> g_tid;
HWND g_wnd;
float g_dpiK = 1;

// o cursor que está crescendo
struct Kind { DWORD ocr; const wchar_t* reg; LPCWSTR idc; };
const Kind KINDS[] = { { 32512, L"Arrow", IDC_ARROW }, { 32513, L"IBeam", IDC_IBEAM }, { 32649, L"Hand", IDC_HAND } };
const Kind* g_kind;
std::wstring g_file;              // arquivo do esquema (vazio: o padrão do Windows)
std::map<int, HCURSOR> g_cache;   // por tamanho (px)
int g_base = 32;                  // tamanho normal (px)
int g_applied = 0;                // tamanho posto agora (0 = o normal do esquema)

float g_mag = 1, g_from = 1, g_target = 1;
double g_animAt = 0, g_animMs = GROW_MS, g_lastShake = 0, g_lastFast = 0;
POINT g_prevPt{}; double g_prevAt = 0;

float InOutCubic(float t) { t = std::clamp(t, 0.f, 1.f); return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }
float OutCubic(float t) { t = std::clamp(t, 0.f, 1.f); return 1 - std::pow(1 - t, 3.f); }

bool Detect(double now, float x, float y) {
    while (!g_hist.empty() && now - g_hist.front().t >= WINDOW_MS) g_hist.pop_front();
    auto same = [](float a, float b) { return (a >= -1 && b >= -1) || (a <= 1 && b <= 1); };  // ±1 px vale qualquer lado
    if (g_hist.size() >= 2) {
        auto& p = g_hist[g_hist.size() - 2]; auto& q = g_hist.back();
        if (same(q.x - p.x, x - q.x) && same(q.y - p.y, y - q.y)) { q = { now, x, y }; return false; }  // trecho reto: só as pontas
    }
    g_hist.push_back({ now, x, y });
    if (g_hist.size() < 4) return false;
    float dist = 0, l = g_hist[0].x, r = l, t = g_hist[0].y, b = t;
    for (size_t i = 1; i < g_hist.size(); i++) {
        dist += std::hypot(g_hist[i].x - g_hist[i - 1].x, g_hist[i].y - g_hist[i - 1].y);
        l = std::min(l, g_hist[i].x); r = std::max(r, g_hist[i].x); t = std::min(t, g_hist[i].y); b = std::max(b, g_hist[i].y);
    }
    float diag = std::hypot(r - l, b - t);
    if (diag < MIN_DIAG * g_dpiK) return false;
    if (dist / diag > SENSITIVITY) { g_hist.clear(); return true; }
    return false;
}

const Kind* CurrentKind() {
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return nullptr;  // escondido (jogo, vídeo): nada
    for (auto& k : KINDS) if (LoadCursorW(nullptr, k.idc) == ci.hCursor) return &k;
    return nullptr;  // cursor próprio de um app: não é do esquema
}

HCURSOR Load(int size) {
    auto f = g_cache.find(size);
    if (f != g_cache.end()) return f->second;
    HCURSOR c = g_file.empty()
        ? (HCURSOR)LoadImageW(nullptr, MAKEINTRESOURCEW(g_kind->ocr), IMAGE_CURSOR, size, size, 0)
        : (HCURSOR)LoadImageW(nullptr, g_file.c_str(), IMAGE_CURSOR, size, size, LR_LOADFROMFILE);
    g_cache[size] = c;
    return c;
}

void ClearCache() { for (auto& [s, c] : g_cache) if (c) DestroyCursor(c); g_cache.clear(); }

void RestoreScheme() {
    if (!g_applied) return;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // relê o esquema do registro (não grava nada)
    g_applied = 0;
}

void Apply(float mag) {
    if (!g_kind) return;
    int size = (int)std::lround(g_base * mag / 2) * 2;
    if (mag <= 1.001f || size <= g_base) { RestoreScheme(); return; }
    if (size == g_applied) return;
    HCURSOR c = Load(size);
    if (!c) return;
    if (SetSystemCursor(CopyCursor(c), g_kind->ocr)) g_applied = size;  // o sistema fica com a cópia
}

void Begin() {  // primeira sacudida: qual cursor e de onde vem
    g_kind = CurrentKind();
    if (!g_kind) return;
    wchar_t raw[MAX_PATH] = {}, path[MAX_PATH] = {}; DWORD cb = sizeof(raw);
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", g_kind->reg, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw, &cb);
    ExpandEnvironmentStringsW(raw, path, MAX_PATH);
    std::wstring p = path;
    bool ani = p.size() > 4 && !_wcsicmp(p.c_str() + p.size() - 4, L".ani");
    if (ani) { g_kind = nullptr; return; }  // animado (ocupado): fica como está
    if (p != g_file) ClearCache();
    g_file = p;
    UINT dpi = GetDpiForSystem();
    g_base = GetSystemMetricsForDpi(SM_CXCURSOR, dpi);
    if (g_base < 16) g_base = 32;
}

void Animate(double now) {
    float t = (float)((now - g_animAt) / g_animMs);
    g_mag = g_from + (g_target - g_from) * (g_target < g_from ? OutCubic(t) : InOutCubic(t));  // encolher começa já rápido
    if (t >= 1) g_mag = g_target;
    Apply(g_mag);
}

void AnimateTo(float target, double now) {
    g_from = g_mag; g_target = target; g_animAt = now;
    g_animMs = target > g_mag ? GROW_MS : SHRINK_MS;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        RAWINPUT ri; UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE) {
            double now = anim::Now();
            // botão segurado (arrastando, desenhando): não é sacudida
            if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0) { g_hist.clear(); return 0; }
            POINT p; GetCursorPos(&p);
            if (g_prevAt <= 0 || now - g_prevAt >= 4) {  // velocidade a cada ~4 ms (mouses de 1000+ Hz mandam bem mais)
                if (g_prevAt > 0 && now - g_prevAt < 100) {
                    float v = (float)(std::hypot((double)p.x - g_prevPt.x, (double)p.y - g_prevPt.y) / (now - g_prevAt));
                    if (v > FAST * g_dpiK) g_lastFast = now;
                }
                g_prevPt = p; g_prevAt = now;
            }
            if (Detect(now, (float)p.x, (float)p.y)) {
                if (g_target <= 1.f) Begin();
                if (g_kind) {
                    float target = g_target <= 1.f ? FIRST : std::min(MAXMAG, g_target + STEP);
                    if (target != g_target) AnimateTo(target, now);
                    g_lastShake = now; g_lastFast = now;
                }
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

void Thread() {
    g_tid = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpiK = GetDpiForSystem() / 96.f;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ShellPonteiro";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{ 0x01, 0x02, RIDEV_INPUTSINK, g_wnd };  // mouse, mesmo sem foco
    if (!g_wnd || !RegisterRawInputDevices(&rid, 1, sizeof(rid))) { Log(L"ponteiro: sem raw input (%lu)", GetLastError()); return; }
    Log(L"ponteiro: sacudir para localizar ligado");
    MSG msg;
    for (;;) {
        bool busy = g_mag != g_target || g_target > 1.f;
        if (!busy) {  // parado: só acorda com mouse (nenhum custo)
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            DispatchMessageW(&msg);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto out; DispatchMessageW(&msg); }
        double now = anim::Now();
        if (g_target > 1.f && (now - g_lastFast > SLOW_MS || now - g_lastShake > HOLD_MS)) { AnimateTo(1.f, now); g_hist.clear(); }  // desacelerou: encolhe já
        if (g_mag != g_target) { Animate(now); DwmFlush(); }  // um passo por quadro da tela
        else MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);  // grande e parado: espera o tempo ou o mouse
        if (g_mag == 1.f && g_target == 1.f) { RestoreScheme(); g_kind = nullptr; }
    }
out:
    RestoreScheme();
    ClearCache();
}

}  // namespace

void ponteiro::Start() {
    if (shell::Setting(L"sacudir_ponteiro", L"1") != L"1") return;
    std::thread(Thread).detach();
}

void ponteiro::Stop() {
    if (DWORD t = g_tid) PostThreadMessageW(t, WM_QUIT, 0, 0);
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // o cursor volta mesmo se a thread não chegar a tempo
}
