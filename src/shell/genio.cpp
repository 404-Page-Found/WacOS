// Efeito gênio do Mac ao minimizar (e ao contrário ao restaurar pelo Dock).
// A miniatura viva de uma janela (DWM) só pode ser um retângulo; então a janela é fatiada em faixas
// horizontais, cada uma uma miniatura com a sua largura e posição: juntas desenham o funil. Duas etapas,
// como no Mac: o pé da janela estreita em direção ao lugar no Dock e a janela escorre funil abaixo
// (as faixas de baixo chegam primeiro).
// Sem quadros vazios: antes de minimizar, o Windows pergunta à "barra de tarefas" para onde a janela vai
// (HSHELL_GETMINRECT, com a janela ainda na tela). Nessa hora o Shell cobre a tela com uma cópia viva dela
// (a mesa e as outras janelas, miniaturas do DWM) e o gênio por cima; o que o Windows faz embaixo (a
// animação dele, a janela sumindo ou voltando e redesenhando) fica escondido. A configuração de animação
// do Windows não é mexida.
#include "shell.h"
#include <dwmapi.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace {

const int MAX_STRIPS = 120;   // uma faixa a cada ~5 px de janela: a curva do funil sem degraus
const float DURATION = 0.42f;      // s — com a curva padrão do Mac, que já sai andando (sem o começo parado)
const UINT WM_GN_FRAME = WM_APP + 90;

struct Anim {
    HWND target = nullptr;
    RECT win{}, slot{};            // tela: a janela (parte visível) e o lugar no Dock
    RECT src{};                    // parte da janela usada (sem a borda invisível)
    int inset = 0; bool maxed = false;
    bool restore = false;          // true: do Dock para a tela
    double t0 = 0;
    int n = 48;                    // faixas desta janela
    std::vector<HTHUMBNAIL> strips;
    bool realBack = false;         // restaurar: a janela de verdade já voltou (atrás da camada)
    std::vector<HTHUMBNAIL> replica;  // a cópia da tela por baixo do gênio (mesa e as outras janelas)
    double settleAt = 0;           // restaurar: esperando a janela redesenhar antes de tirar a camada
};

HWND g_ov;
RECT g_ovRect{};
Anim g_a;
bool g_running;
HWINEVENTHOOK g_hook;
BOOL g_prevAnim = TRUE;            // como estava a animação do Windows antes do Shell
bool g_changedAnim;

float Clamp01(float x) { return std::clamp(x, 0.f, 1.f); }
float Smooth(float e0, float e1, float x) { float t = Clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }

// restaurar: a janela de verdade volta escondida atrás da camada (o app tem tempo de redesenhar; antes ela
// aparecia sumida e depois preta por alguns quadros)
void RealBack() {
    if (g_a.realBack || !IsWindow(g_a.target)) return;
    g_a.realBack = true;
    shell::Restaurar(g_a.target);
    keybd_event(VK_MENU, 0, 0, 0); SetForegroundWindow(g_a.target); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // a camada continua por cima
    dock::Raise();
}

void Finish() {
    for (auto t : g_a.strips) if (t) DwmUnregisterThumbnail(t);
    for (auto t : g_a.replica) if (t) DwmUnregisterThumbnail(t);
    g_a.strips.clear(); g_a.replica.clear();
    ShowWindow(g_ov, SW_HIDE);
    ui::FrameClock(g_ov, WM_GN_FRAME, false);
    g_running = false;
    HWND h = g_a.target;
    bool restore = g_a.restore;
    bool realBack = g_a.realBack;
    g_a.target = nullptr;
    if (restore && IsWindow(h) && !realBack) { shell::Restaurar(h); SetForegroundWindow(h); }
    apps::Refresh();
    dock::Update();  // a miniatura entra (ou sai) do Dock só agora
}


// Onde fica a faixa [v0, v1] da janela (v = 0 no alto, 1 no pé) no instante p (0 = janela na tela, 1 = no Dock)
void StripAt(float p, float v0, float v1, float& L, float& R, float& ya, float& yb) {
    const RECT& W = g_a.win; const RECT& S = g_a.slot;
    float wH = (float)(W.bottom - W.top), sH = (float)(S.bottom - S.top);
    float down = (float)S.top - (float)W.top;
    float funnel = Smooth(0.f, 0.35f, p);
    auto Ypos = [&](float v) {
        float a = Smooth(0.05f + (1 - v) * 0.35f, 1.f, p);
        return W.top + v * wH + ((S.top + v * sH) - (W.top + v * wH)) * a;
    };
    ya = Ypos(v0); yb = Ypos(v1);
    float ym = (ya + yb) / 2;
    float c = down > 1 ? Smooth((float)W.top, (float)S.top, ym) : 1.f;
    float k = c * funnel;
    L = W.left + (S.left - W.left) * k; R = W.right + (S.right - W.right) * k;
}

// ---------------------------------------------------------------- motor novo: o compositor anima
// O Shell calcula o caminho de cada faixa uma vez só e entrega ao DirectComposition; daí em diante quem
// anima é o compositor do Windows (DWM), na placa de vídeo, sem passar pela thread do Shell: um Shell
// ocupado não faz a animação perder quadro.
// A imagem é uma foto da janela: a miniatura do DWM (que continua valendo com a janela já minimizada)
// é posta numa janela nossa e fotografada com PrintWindow (PW_RENDERFULLCONTENT); a foto vira uma
// superfície do compositor e cada faixa mostra um pedaço dela. Para não ter quadro vazio ao minimizar, a
// janela da foto fica na tela, no lugar da janela, até o compositor entrar. Se algo falhar, o motor antigo
// (quadro a quadro) assume. Ajuste: genio_motor=composicao (padrão) ou antigo.
namespace comp {
ID3D11Device* g_d3d;
ID3D11DeviceContext* g_ctx;
IDCompositionDesktopDevice* g_dev;
IDCompositionTarget* g_target;
IDCompositionVisual2* g_root;
// Níveis de detalhe: a foto inteira e cópias com metade, um quarto... da LARGURA (a altura fica), cada uma a
// média de 2 pixels vizinhos da anterior. Só na largura porque o funil estreita a janela muito antes de achatá-la:
// reduzir as duas direções juntas trazia, numa janela fina mas ainda alta, linhas esticadas em blocos. Na altura,
// as faixas finas (cada uma com poucas linhas) já fazem a média. O compositor só filtra 2×2 pixels: encolhendo a foto inteira até ~5% ele pulava linhas e
// colunas e o gênio ficava serrilhado e "pixelado" (Reddit, 20/11/2026). Cada nível tem as suas faixas (o de baixo,
// menor, com menos) e a de cima some quando a sua escala passa de ~0,6 para 0,5, mostrando a de baixo.
const int MAX_LEVELS = 6;
IDCompositionSurface* g_surf[MAX_LEVELS];
IDCompositionVisual2* g_level[MAX_LEVELS];  // um grupo por nível; o nível 0 (a foto inteira) por cima
int g_levels;
IDCompositionEffectGroup* g_fx;
HWND g_wnd, g_cap;                 // a camada do compositor e a janela da foto
HTHUMBNAIL g_capTh;
bool g_ok, g_tried, g_active;
struct Strip { IDCompositionVisual2* v = nullptr; IDCompositionScaleTransform* sc = nullptr; IDCompositionEffectGroup* fx = nullptr; int level = 0; };
std::vector<Strip> g_strips;
const UINT_PTR T_BACK = 1, T_END = 2, T_SETTLE = 3;

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);

bool Init(HINSTANCE hi) {
    if (g_tried) return g_ok;
    g_tried = true;
    // o mesmo dispositivo da GPU do resto do Shell (ui::D3D), não um só para o gênio: menos memória de vídeo e
    // um driver a menos inicializando. O gênio roda na thread principal, a mesma dos outros desenhos.
    g_d3d = ui::D3D();
    if (!g_d3d) return false;
    g_d3d->AddRef();
    g_d3d->GetImmediateContext(&g_ctx);
    IDXGIDevice* dx = nullptr; g_d3d->QueryInterface(IID_PPV_ARGS(&dx));
    HRESULT hr = dx ? DCompositionCreateDevice3(dx, IID_PPV_ARGS(&g_dev)) : E_FAIL;
    if (dx) dx->Release();
    if (FAILED(hr) || !g_dev) { Log(L"gênio (compositor): sem dispositivo (%08X)", (unsigned)hr); return false; }
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellGenioComposicao";
    RegisterClassW(&wc);
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;
    g_wnd = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    g_cap = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_cap) return false;
    apps::ours.push_back(g_wnd); apps::ours.push_back(g_cap);
    if (FAILED(g_dev->CreateTargetForHwnd(g_wnd, TRUE, &g_target)) || FAILED(g_dev->CreateVisual(&g_root))) return false;
    g_dev->CreateEffectGroup(&g_fx);
    if (g_fx) g_root->SetEffect(g_fx);
    g_root->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    // borda suave: o contorno do funil sai suavizado, não em escadinha. Para não aparecer fresta entre as faixas,
    // cada uma desce 2 linhas por baixo da seguinte e fica por cima dela (Start)
    g_root->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
    g_target->SetRoot(g_root);
    g_dev->Commit();
    g_ok = true;
    Log(L"gênio (compositor): pronto");
    return true;
}

// curva suave pelos pontos (Hermite com as inclinações de Catmull-Rom): o compositor interpola sozinho.
// Linear nos valores: faixas vizinhas, que dividem a borda nos pontos, continuam encostadas entre eles.
IDCompositionAnimation* Curve(const std::vector<float>& v, float dur) {
    IDCompositionAnimation* an = nullptr;
    if (FAILED(g_dev->CreateAnimation(&an))) return nullptr;
    int K = (int)v.size();
    float dt = dur / (K - 1);
    auto slope = [&](int k) {
        if (k == 0) return (v[1] - v[0]) / dt;
        if (k == K - 1) return (v[K - 1] - v[K - 2]) / dt;
        return (v[k + 1] - v[k - 1]) / (2 * dt);
    };
    for (int k = 0; k < K - 1; k++) {
        float p0 = v[k], p1 = v[k + 1], m0 = slope(k), m1 = slope(k + 1), d = (p1 - p0) / dt;
        an->AddCubic(k * dt, p0, m0, (3 * d - 2 * m0 - m1) / dt, (m0 + m1 - 2 * d) / (dt * dt));
    }
    an->End(dur, v[K - 1]);
    return an;
}

void HideCap() {
    if (g_capTh) { DwmUnregisterThumbnail(g_capTh); g_capTh = nullptr; }
    ShowWindow(g_cap, SW_HIDE);
}

void Clear() {
    for (auto& s : g_strips) {
        if (s.v) { if (g_level[s.level]) g_level[s.level]->RemoveVisual(s.v); s.v->Release(); }
        if (s.sc) s.sc->Release();
        if (s.fx) s.fx->Release();
    }
    g_strips.clear();
    for (int k = 0; k < MAX_LEVELS; k++) {
        if (g_level[k]) { g_root->RemoveVisual(g_level[k]); g_level[k]->Release(); g_level[k] = nullptr; }
        if (g_surf[k]) { g_surf[k]->Release(); g_surf[k] = nullptr; }
    }
    g_levels = 0;
    g_dev->Commit();
    HideCap();
}

void FinishComp() {
    KillTimer(g_wnd, T_BACK); KillTimer(g_wnd, T_END); KillTimer(g_wnd, T_SETTLE);
    Clear();
    ShowWindow(g_wnd, SW_HIDE);
    g_active = false;
    g_running = false;
    HWND h = g_a.target;
    bool restore = g_a.restore, realBack = g_a.realBack;
    g_a.target = nullptr;
    if (restore && IsWindow(h) && !realBack) { shell::Restaurar(h); SetForegroundWindow(h); }
    apps::Refresh();
    dock::Update();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_TIMER && h == g_wnd) {
        if (w == T_BACK) { KillTimer(h, T_BACK); RealBack(); SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); dock::Raise(); }
        else if (w == T_END) { KillTimer(h, T_END); if (g_a.restore) { RealBack(); SetTimer(h, T_SETTLE, 140, nullptr); } else FinishComp(); }
        else if (w == T_SETTLE) FinishComp();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// A foto da janela (miniatura do DWM numa janela nossa). show: a janela da foto aparece no lugar da janela
// (ao minimizar, cobre o instante em que ela já sumiu e o compositor ainda não entrou).
bool Snap(HWND h, bool show, std::vector<uint32_t>& px, int& w, int& hh) {
    if (g_capTh) HideCap();
    if (FAILED(DwmRegisterThumbnail(g_cap, h, &g_capTh))) { g_capTh = nullptr; return false; }
    SIZE sz{};
    if (SUCCEEDED(DwmQueryThumbnailSourceSize(g_capTh, &sz)) && sz.cx > 2 * g_a.inset && sz.cy > 2 * g_a.inset)
        g_a.src = { g_a.inset, g_a.maxed ? g_a.inset : 0, sz.cx - g_a.inset, sz.cy - g_a.inset };
    w = g_a.src.right - g_a.src.left; hh = g_a.src.bottom - g_a.src.top;
    if (w < 4 || hh < 4 || w > 16384 || hh > 16384) { HideCap(); return false; }
    BOOL cloak = show ? FALSE : TRUE;
    DwmSetWindowAttribute(g_cap, DWMWA_CLOAK, &cloak, sizeof(cloak));
    SetWindowPos(g_cap, HWND_TOPMOST, g_a.win.left, g_a.win.top, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    DWM_THUMBNAIL_PROPERTIES tp{};
    tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    tp.rcSource = g_a.src; tp.rcDestination = { 0, 0, w, hh };
    tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties(g_capTh, &tp);
    if (show) dock::Raise();
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB };
    void* bits = nullptr; HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool got = false;
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        for (int tries = 0; tries < 2 && !got; tries++) {
            if (tries) DwmFlush();
            if (!PrintWindow(g_cap, mdc, PW_RENDERFULLCONTENT)) continue;
            GdiFlush();
            const uint32_t* p = (const uint32_t*)bits;
            for (int y = 0; y < hh && !got; y += 7) for (int x = 0; x < w; x += 7) if (p[y * w + x] & 0xFFFFFF) { got = true; break; }
        }
        if (got) {
            px.assign((const uint32_t*)bits, (const uint32_t*)bits + (size_t)w * hh);
            // opaca, com os cantos arredondados das janelas do Windows 11 (maximizada não tem)
            UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
            float r = g_a.maxed ? 0.f : 8.f * dpi / 96;
            for (auto& c : px) c |= 0xFF000000;
            if (r > 0) {
                int R = (int)std::ceil(r);
                for (int y = 0; y < R && y < hh; y++) for (int x = 0; x < R && x < w; x++) {
                    float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
                    float a = std::clamp(r + 0.5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
                    if (a >= 1) continue;
                    auto fade = [&](uint32_t& c) {
                        uint32_t b = (uint32_t)((c & 0xFF) * a), g = (uint32_t)(((c >> 8) & 0xFF) * a), rr = (uint32_t)(((c >> 16) & 0xFF) * a);
                        c = ((uint32_t)(255 * a) << 24) | (rr << 16) | (g << 8) | b;
                    };
                    fade(px[(size_t)y * w + x]); fade(px[(size_t)y * w + (w - 1 - x)]);
                    fade(px[(size_t)(hh - 1 - y) * w + x]); fade(px[(size_t)(hh - 1 - y) * w + (w - 1 - x)]);
                }
            }
        }
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    if (!got) { HideCap(); return false; }
    if (!show) HideCap();
    return true;
}

bool Upload(const std::vector<uint32_t>& px, int w, int hh, IDCompositionSurface** out) {
    if (FAILED(g_dev->CreateSurface(w, hh, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, out))) return false;
    ID3D11Texture2D* tex = nullptr; POINT off{};
    if (FAILED((*out)->BeginDraw(nullptr, IID_PPV_ARGS(&tex), &off))) return false;
    D3D11_BOX box{ (UINT)off.x, (UINT)off.y, 0, (UINT)(off.x + w), (UINT)(off.y + hh), 1 };
    g_ctx->UpdateSubresource(tex, 0, &box, px.data(), w * 4, 0);
    tex->Release();
    return SUCCEEDED((*out)->EndDraw());
}

// metade da largura: cada pixel é a média dos 2 vizinhos (cores pré-multiplicadas: a média vale para o alfa também)
void Half(const std::vector<uint32_t>& in, int w, int h, std::vector<uint32_t>& out, int& ow, int& oh) {
    ow = std::max(1, (w + 1) / 2); oh = h;
    out.assign((size_t)ow * oh, 0);
    for (int y = 0; y < oh; y++) {
        const uint32_t* row = &in[(size_t)y * w];
        uint32_t* dst = &out[(size_t)y * ow];
        for (int x = 0; x < ow; x++) {
            uint32_t a = row[2 * x], b = row[std::min(2 * x + 1, w - 1)];
            // média byte a byte sem separar os canais
            dst[x] = (a | b) - (((a ^ b) & 0xFEFEFEFEu) >> 1);
        }
    }
}

// true: o compositor está animando (o motor antigo não roda)
bool Start(HWND h) {
    if (!g_ok) return false;
    double t0 = anim::Now();
    std::vector<uint32_t> px; int sw = 0, sh = 0;
    if (!Snap(h, !g_a.restore, px, sw, sh)) { Log(L"gênio (compositor): sem foto da janela; motor antigo"); return false; }
    double tSnap = anim::Now() - t0;
    // os níveis: a foto inteira e as metades, até ~16 px ou MAX_LEVELS
    std::vector<uint32_t> lv = std::move(px); int lw = sw, lh = sh;
    int dims[MAX_LEVELS][2]{};
    for (g_levels = 0; g_levels < MAX_LEVELS; g_levels++) {
        if (!Upload(lv, lw, lh, &g_surf[g_levels])) break;
        dims[g_levels][0] = lw; dims[g_levels][1] = lh;
        if (lw < 24) { g_levels++; break; }
        std::vector<uint32_t> nx; int nw, nh; Half(lv, lw, lh, nx, nw, nh);
        lv = std::move(nx); lw = nw; lh = nh;
    }
    if (!g_levels) { Log(L"gênio (compositor): superfície falhou; motor antigo"); Clear(); return false; }
    // grupos de cada nível: o de baixo (o menor) primeiro, cada um por cima do anterior
    for (int k = g_levels - 1; k >= 0; k--) {
        if (FAILED(g_dev->CreateVisual(&g_level[k]))) { Clear(); return false; }
        g_root->AddVisual(g_level[k], TRUE, k + 1 < g_levels ? g_level[k + 1] : nullptr);
    }
    SetWindowPos(g_wnd, HWND_TOPMOST, g_ovRect.left, g_ovRect.top, g_ovRect.right - g_ovRect.left, g_ovRect.bottom - g_ovRect.top, SWP_NOACTIVATE);
    const float D = DURATION;
    const int K = 32;
    // faixas finas (~3 px de janela) no nível 0: a curva do funil sem degraus; cada nível menor tem a metade
    const int n0 = std::clamp((int)((g_a.win.bottom - g_a.win.top) / 3), 24, 200);
    int total = 0;
    for (int lvl = 0; lvl < g_levels; lvl++) {
        const int n = lvl == 0 ? n0 : std::max(12, n0 / 2);  // os níveis estreitos aparecem com a janela já fina
        IDCompositionVisual2* prev = nullptr;  // cada faixa entra por baixo da anterior (a de cima cobre a emenda)
        const float lw2 = (float)dims[lvl][0], lh2 = (float)dims[lvl][1];
        const float mult = (float)(1 << lvl);  // escala da foto inteira -> escala deste nível
        for (int i = 0; i < n; i++) {
            float v0 = (float)i / n, v1 = (float)(i + 1) / n;
            float top = v0 * lh2, bot = v1 * lh2;          // as linhas da foto (deste nível) desta faixa
            Strip st; st.level = lvl;
            if (FAILED(g_dev->CreateVisual(&st.v))) { Clear(); return false; }
            st.v->SetContent(g_surf[lvl]);
            st.v->SetClip(D2D_RECT_F{ 0.f, top, lw2, i + 1 < n ? std::min(lh2, bot + 2.f) : bot });  // 2 linhas por baixo da seguinte
            std::vector<float> ox(K), oy(K), sx(K), sy(K), op(K);
            bool fades = false;
            for (int k = 0; k < K; k++) {
                float t = (float)k / (K - 1);
                float pp = g_a.restore ? 1 - anim::Ease(t) : anim::Ease(t);
                float L, R, ya, yb; StripAt(pp, v0, v1, L, R, ya, yb);
                sx[k] = std::max(0.f, (R - L) / lw2);
                sy[k] = std::max(0.f, (yb - ya) / (bot - top));
                ox[k] = L - g_ovRect.left;
                oy[k] = ya - g_ovRect.top - top * sy[k];   // a linha `top` da foto cai em ya
                // escala deste nível na tela: abaixo de ~0,5 o filtro do compositor pula pixels; o nível menor assume
                float m = sx[k];  // os níveis só reduzem a largura
                op[k] = lvl + 1 < g_levels ? Smooth(0.5f, 0.65f, m) : 1.f;
                if (op[k] < 0.999f) fades = true;
                (void)mult;
            }
            g_dev->CreateScaleTransform(&st.sc);
            IDCompositionAnimation *ax = Curve(ox, D), *ay = Curve(oy, D), *asx = Curve(sx, D), *asy = Curve(sy, D);
            if (ax) { st.v->SetOffsetX(ax); ax->Release(); }
            if (ay) { st.v->SetOffsetY(ay); ay->Release(); }
            if (st.sc) {
                st.sc->SetCenterX(0.f); st.sc->SetCenterY(0.f);
                if (asx) { st.sc->SetScaleX(asx); asx->Release(); }
                if (asy) { st.sc->SetScaleY(asy); asy->Release(); }
                st.v->SetTransform(st.sc);
            }
            if (fades && SUCCEEDED(g_dev->CreateEffectGroup(&st.fx)) && st.fx) {
                if (IDCompositionAnimation* ao = Curve(op, D)) { st.fx->SetOpacity(ao); ao->Release(); }
                st.v->SetEffect(st.fx);
            }
            g_level[lvl]->AddVisual(st.v, FALSE, prev);
            prev = st.v;
            g_strips.push_back(st);
            total++;
        }
    }
    const int n = total;
    if (g_fx) {  // some de leve ao entrar no Dock (igual ao motor antigo)
        std::vector<float> op(K);
        for (int k = 0; k < K; k++) {
            float t = (float)k / (K - 1), pp = g_a.restore ? 1 - anim::Ease(t) : anim::Ease(t);
            op[k] = 1 - 0.15f * Smooth(0.85f, 1.f, pp);
        }
        if (IDCompositionAnimation* ao = Curve(op, D)) { g_fx->SetOpacity(ao); ao->Release(); }
    }
    g_running = true; g_active = true;
    g_a.t0 = anim::Now();
    g_dev->Commit();
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);  // por cima da foto
    dock::Raise();
    if (!g_a.restore) { DwmFlush(); HideCap(); }  // o compositor já está na tela: a foto parada sai
    UINT ms = (UINT)std::lround(D * 1000);
    if (g_a.restore) SetTimer(g_wnd, T_BACK, (UINT)(ms * 0.4f), nullptr);
    SetTimer(g_wnd, T_END, ms + 20, nullptr);
    Log(L"gênio (compositor): %d faixas em %d níveis, foto %dx%d em %.1f ms, preparo %.1f ms; o resto é com o DWM", n, g_levels, sw, sh, tSnap, anim::Now() - t0);
    dock::Update();
    return true;
}
}  // namespace comp

// Uma faixa de cada vez: v = 0 no alto da janela, 1 no pé.
void Frame() {
    if (g_a.settleAt) { if (anim::Now() - g_a.settleAt > 140) Finish(); return; }  // a janela já redesenhou atrás
    float t = std::min(1.f, (float)((anim::Now() - g_a.t0) / 1000.0) / DURATION);
    float p = g_a.restore ? 1 - anim::Ease(t) : anim::Ease(t);  // 0 = janela na tela, 1 = no Dock
    if (g_a.restore && t >= 0.4f) RealBack();  // volta cedo, escondida: tem tempo de redesenhar (e a animação do Windows acaba)
    const RECT& W = g_a.win; const RECT& S = g_a.slot;
    float wH = (float)(W.bottom - W.top), sH = (float)(S.bottom - S.top);
    float down = (float)S.top - (float)W.top;     // distância do alto da janela ao Dock
    // etapa 1 (0..0,45): o funil se forma — o pé estreita para a largura do lugar
    float funnel = Smooth(0.f, 0.35f, p);
    // etapa 2 (0,15..1): a janela desce; faixas de baixo primeiro
    const int STRIPS = g_a.n;
    for (int i = 0; i < STRIPS; i++) {
        float v0 = (float)i / STRIPS, v1 = (float)(i + 1) / STRIPS;
        auto Ypos = [&](float v) {
            float a = Smooth(0.05f + (1 - v) * 0.35f, 1.f, p);           // quanto esta altura já desceu
            float y0 = W.top + v * wH;                                    // na janela
            float y1 = S.top + v * sH;                                    // no lugar do Dock
            return y0 + (y1 - y0) * a;
        };
        float ya = Ypos(v0), yb = Ypos(v1);
        // largura pela altura na tela: curva em S entre a largura da janela (no alto) e a do lugar (embaixo)
        float ym = (ya + yb) / 2;
        float c = down > 1 ? Smooth((float)W.top, (float)S.top, ym) : 1.f;
        float k = c * funnel;                       // sem funil no começo; a faixa estreita conforme desce
        float L = W.left + (S.left - W.left) * k, R = W.right + (S.right - W.right) * k;
        DWM_THUMBNAIL_PROPERTIES tp{};
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        int sh = g_a.src.bottom - g_a.src.top;
        tp.rcSource = { g_a.src.left, g_a.src.top + (LONG)std::lround(v0 * sh), g_a.src.right, g_a.src.top + (LONG)std::lround(v1 * sh) };
        tp.rcDestination = { (LONG)std::lround(L) - g_ovRect.left, (LONG)std::floor(ya) - g_ovRect.top, (LONG)std::lround(R) - g_ovRect.left, (LONG)std::ceil(yb) - g_ovRect.top };
        tp.fVisible = yb - ya >= 0.2f && R - L >= 1;
        tp.opacity = (BYTE)std::lround(255 * (1 - 0.15f * Smooth(0.85f, 1.f, p)));  // some de leve ao entrar no Dock
        tp.fSourceClientAreaOnly = FALSE;
        if (i < (int)g_a.strips.size() && g_a.strips[i]) DwmUpdateThumbnailProperties(g_a.strips[i], &tp);
    }
    if (t >= 1) {
        if (g_a.restore) { RealBack(); g_a.settleAt = anim::Now(); }  // último quadro fica até a janela redesenhar
        else Finish();
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_GN_FRAME) { if (g_running) Frame(); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// retângulo visível da janela na tela, mesmo minimizada (posição normal ou maximizada)
bool WindowArea(HWND h, RECT& vis, RECT& src) {
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (!GetWindowPlacement(h, &wp)) return false;
    HMONITOR mon = MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    RECT full = wp.rcNormalPosition;  // coordenadas da área de trabalho -> tela
    OffsetRect(&full, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    bool maxed = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
    UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
    int border = maxed ? 0 : MulDiv(7, dpi, 96);  // a borda invisível das janelas normais (lados e embaixo)
    if (maxed) vis = mi.rcWork;
    else vis = { full.left + border, full.top, full.right - border, full.bottom - border };
    // a miniatura tem o tamanho da janela inteira: a fonte é a parte visível dela
    int fw = maxed ? (mi.rcWork.right - mi.rcWork.left) + 2 * MulDiv(8, dpi, 96) : full.right - full.left;
    int fh = maxed ? (mi.rcWork.bottom - mi.rcWork.top) + 2 * MulDiv(8, dpi, 96) : full.bottom - full.top;
    int inset = maxed ? MulDiv(8, dpi, 96) : border;
    src = { inset, maxed ? inset : 0, fw - inset, fh - inset };
    g_a.inset = inset; g_a.maxed = maxed;
    return vis.right > vis.left && vis.bottom > vis.top;
}

// cópia viva da tela por baixo do gênio: a mesa e as janelas visíveis (de baixo para cima), menos a que anima
void BuildReplica(HWND except) {
    std::vector<HWND> desk, wins;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == except || w == g_ov || !IsWindowVisible(w) || IsIconic(w)) continue;
        wchar_t cls[32] = {}; GetClassNameW(w, cls, 32);
        if (mesa::IsDeskClass(cls)) { desk.push_back(w); continue; }
        if (apps::IsShellWindow(w)) continue;  // Dock e barra de menus ficam de verdade, por cima
        DWORD cloaked = 0; DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        RECT r; GetWindowRect(w, &r), (void)r;
        RECT x; if (!IntersectRect(&x, &r, &g_ovRect)) continue;
        wins.push_back(w);
    }
    auto add = [&](HWND w) {
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(g_ov, w, &th))) return;
        RECT wr; GetWindowRect(w, &wr);
        RECT vis = wr; DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis));  // sem a borda invisível
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcSource = { vis.left - wr.left, vis.top - wr.top, vis.right - wr.left, vis.bottom - wr.top };
        p.rcDestination = { vis.left - g_ovRect.left, vis.top - g_ovRect.top, vis.right - g_ovRect.left, vis.bottom - g_ovRect.top };
        p.fVisible = TRUE; p.opacity = 255; p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(th, &p);
        g_a.replica.push_back(th);
    };
    for (auto it = desk.rbegin(); it != desk.rend(); ++it) add(*it);
    for (auto it = wins.rbegin(); it != wins.rend(); ++it) add(*it);
}

void Start(HWND h, RECT slot, bool restore) {
    if (g_running) { if (comp::g_active) comp::FinishComp(); else Finish(); }  // uma de cada vez
    RECT vis, src;
    if (!WindowArea(h, vis, src) || slot.right <= slot.left) {
        if (restore) { shell::Restaurar(h); SetForegroundWindow(h); }
        return;
    }
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromRect(&vis, MONITOR_DEFAULTTONEAREST), &mi);
    g_ovRect = mi.rcMonitor;
    int inset = g_a.inset; bool maxed = g_a.maxed;  // calculados pelo WindowArea
    g_a = Anim{};
    g_a.target = h; g_a.win = vis; g_a.slot = slot; g_a.src = src; g_a.restore = restore; g_a.inset = inset; g_a.maxed = maxed;
    g_a.n = std::clamp((int)((vis.bottom - vis.top) / 5), 24, MAX_STRIPS);
    if (shell::Setting(L"genio_motor", L"composicao") == L"composicao" && comp::Start(h)) return;
    // a fonte real (tamanho da miniatura) manda: corrige a fatia se a janela for de outro tamanho
    SetWindowPos(g_ov, HWND_TOPMOST, g_ovRect.left, g_ovRect.top, g_ovRect.right - g_ovRect.left, g_ovRect.bottom - g_ovRect.top, SWP_NOACTIVATE);
    HTHUMBNAIL whole = nullptr;
    if (!restore && IsIconic(h) && SUCCEEDED(DwmRegisterThumbnail(g_ov, h, &whole))) {  // já sumiu (sem a pergunta do Windows): a janela inteira na hora  // minimizar: primeiro a janela inteira, já
        DWM_THUMBNAIL_PROPERTIES tp{};
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        tp.rcSource = src;
        tp.rcDestination = { vis.left - g_ovRect.left, vis.top - g_ovRect.top, vis.right - g_ovRect.left, vis.bottom - g_ovRect.top };
        tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(whole, &tp);
        ShowWindow(g_ov, SW_SHOWNOACTIVATE);
        dock::Raise();
    }
    g_a.n = std::clamp((int)((vis.bottom - vis.top) / 5), 24, MAX_STRIPS);
    for (int i = 0; i < g_a.n; i++) {
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(g_ov, h, &th))) { th = nullptr; }
        g_a.strips.push_back(th);
    }
    if (!g_a.strips.empty() && g_a.strips[0]) {  // o tamanho de verdade da miniatura manda no recorte
        SIZE sz{};
        if (SUCCEEDED(DwmQueryThumbnailSourceSize(g_a.strips[0], &sz)) && sz.cx > 2 * g_a.inset && sz.cy > 2 * g_a.inset)
            g_a.src = { g_a.inset, g_a.maxed ? g_a.inset : 0, sz.cx - g_a.inset, sz.cy - g_a.inset };
    }
    g_running = true;
    g_a.t0 = anim::Now();
    Frame();
    ShowWindow(g_ov, SW_SHOWNOACTIVATE);
    if (whole) { DwmFlush(); DwmUnregisterThumbnail(whole); }  // as faixas já estão no lugar
    dock::Raise();  // o Dock por cima: a janela entra nele
    ui::FrameClock(g_ov, WM_GN_FRAME, true);
    dock::Update();  // o Dock já abre (ou fecha) o espaço, aos poucos, enquanto a janela anda
}

bool Wanted(HWND h) {
    if (!h || apps::IsShellWindow(h) || GetAncestor(h, GA_ROOT) != h) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || !(st & WS_CAPTION)) return false;
    return true;
}

DWORD g_pauseUntil;  // Win+D: sem gênio até esta hora (genio::Pause)
void CALLBACK OnMinimize(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || ev != EVENT_SYSTEM_MINIMIZESTART || !Wanted(h)) return;
    if (revelar::Owns(h) || apps::IsHidden(h)) return;  // Ocultar: some sem o gênio
    if ((int)(g_pauseUntil - GetTickCount()) > 0) return;  // Win+D: sem o gênio
    if (g_running && g_a.target == h) { Log(L"gênio: aviso de minimizou %.1f ms depois do começo", anim::Now() - g_a.t0); return; }  // já começou pela pergunta do Windows
    RECT slot = dock::MinimizeTarget();
    Start(h, slot, false);
}

}  // namespace

void genio::Create(HINSTANCE hi) {
    if (shell::Setting(L"genio", L"1") != L"1") return;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellGenio";
    RegisterClassW(&wc);
    g_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_ov) return;
    apps::ours.push_back(g_ov);
    // a animação de minimizar do Windows sai de cena enquanto o Shell roda (só nesta sessão, sem gravar).
    // (Tentei manter ligada e cobrir a tela com uma cópia viva dela: apps Chromium/Electron param de
    // desenhar quando se acham cobertos e a cópia ficava preta. Descartado.)
    ANIMATIONINFO ai{ sizeof(ai) };
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0)) {
        g_prevAnim = ai.iMinAnimate;
        if (ai.iMinAnimate) { ai.iMinAnimate = FALSE; SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0); g_changedAnim = true; }
    }
    if (shell::Setting(L"genio_motor", L"composicao") == L"composicao") comp::Init(hi);
    g_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnMinimize, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    Log(L"gênio: ligado (animação do Windows %ls)", g_changedAnim ? L"desligada nesta sessão" : L"já estava desligada");
}

void genio::Stop() {
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    if (g_running) { if (comp::g_active) comp::FinishComp(); else Finish(); }
    if (g_changedAnim) {  // devolve a animação do Windows
        ANIMATIONINFO ai{ sizeof(ai) }; ai.iMinAnimate = g_prevAnim;
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, 0);
        g_changedAnim = false;
    }
}

bool genio::Busy(HWND h) { return g_running && g_a.target == h; }
void genio::Pause(DWORD ms) { g_pauseUntil = GetTickCount() + ms; }

// HSHELL_GETMINRECT: o Windows vai minimizar `h` (ela ainda está na tela). Começa o gênio agora e responde
// com o lugar no Dock. false: não é uma janela nossa de animar (ou é a volta de uma restauração).
bool genio::OnMinRect(HWND h, RECT* out) {
    if (!g_ov || !Wanted(h) || revelar::Owns(h) || IsIconic(h) || apps::IsHidden(h) || (int)(g_pauseUntil - GetTickCount()) > 0) return false;
    if (g_running && g_a.target == h) { *out = g_a.slot; return true; }
    RECT slot = dock::MinimizeTarget();
    if (slot.right <= slot.left) return false;
    double t0 = anim::Now();
    Start(h, slot, false);
    Log(L"gênio: pergunta do Windows (iconic=%d), preparo %.1f ms, %zu janelas na cópia, %d faixas", IsIconic(h) ? 1 : 0, anim::Now() - t0, g_a.replica.size(), g_a.n);
    *out = slot;
    return true;
}
int genio::Phase(HWND h) { return g_running && g_a.target == h ? (g_a.restore ? 2 : 1) : 0; }

// clique na miniatura do Dock: a janela sai do lugar dela (tela) e volta pelo funil
bool genio::Restore(HWND h, RECT fromSlot) {
    if (!g_ov) return false;
    Start(h, fromSlot, true);
    return true;
}


// o dispositivo D3D deste arquivo, para a memória descartável (memoria.cpp: Trim na pressão de memória)
ID3D11Device* GenioD3D() { return comp::g_d3d; }
