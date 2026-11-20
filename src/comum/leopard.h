#pragma once
// Tema Mac OS X Leopard (10.5, 2007): leopard=1 no %APPDATA%\Shell\ajustes.txt, só sem o liquid_glass (o Sequoia
// fica por baixo; o Leopard troca a barra de menus, a Dock, os botões da janela e as barras das janelas).
// Aqui o desenho comum a todos os programas: o cinza metálico das janelas "unified" do Leopard (barra de título e
// barra de ferramentas numa peça só, com um degradê de cima para baixo) e a barra de baixo das janelas.
// Cores medidas pixel a pixel em capturas do 10.5 a 1x (galeria do toastytech.com: Font Book na frente, Finder e
// iCal atrás). A barra "unified" (título + ferramentas) tem 55 px:
//   na frente: fio de luz 220, depois 197 em cima -> 150 embaixo, fio escuro 64 embaixo
//   atrás:     fio de luz 241, depois 233 em cima -> 207 embaixo, fio 135 embaixo
//   barra de baixo (atrás): fio 238, 228 -> 207
// O Leopard não tinha modo escuro: no escuro do Windows o metal vira grafite (o texto e os ícones dos apps seguem
// claros, então continuam legíveis).
#include <d2d1.h>
#include "semaforo.h"

namespace leopard {

inline bool On() { return semaforo::Leopard(); }

// o degradê metálico em r (DIP). top = true: barra de cima (fio escuro embaixo); false: barra de baixo (fio escuro
// em cima). rTop: canto arredondado de cima (barra de título de uma janela com cantos redondos), 0 = reto.
inline void Metal(ID2D1RenderTarget* rt, D2D1_RECT_F r, bool active, bool dark, bool top = true, float rTop = 0) {
    if (!rt || r.bottom <= r.top || r.right <= r.left) return;
    auto G = [](float v, float a = 1) { return D2D1::ColorF(v, v, v, a); };
    D2D1_COLOR_F c0, c1, edge;
    float hl = active ? 220 / 255.f : 241 / 255.f;  // o fio de luz do alto
    if (dark) { c0 = G(active ? 0.36f : 0.30f); c1 = G(active ? 0.22f : 0.24f); edge = G(0.06f); hl = 0.42f; }
    else if (active) { c0 = G(197 / 255.f); c1 = G(150 / 255.f); edge = G(64 / 255.f); }
    else { c0 = G(233 / 255.f); c1 = G(207 / 255.f); edge = G(135 / 255.f); }
    if (!top && !dark) {  // barra de baixo: o mesmo metal (atrás: 228 -> 207, fio 238; na frente proporcional)
        c0 = G(active ? 190 / 255.f : 228 / 255.f); c1 = G(active ? 150 / 255.f : 207 / 255.f); hl = active ? 214 / 255.f : 238 / 255.f;
    }
    ID2D1Factory* f = nullptr; rt->GetFactory(&f);
    ID2D1Geometry* shape = nullptr;
    if (rTop > 0.5f && f) {  // só os cantos de cima arredondados
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(f->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g->Open(&sk))) {
                float R = rTop;
                sk->BeginFigure({ r.left, r.bottom }, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({ r.left, r.top + R });
                sk->AddArc(D2D1::ArcSegment({ r.left + R, r.top }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine({ r.right - R, r.top });
                sk->AddArc(D2D1::ArcSegment({ r.right, r.top + R }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine({ r.right, r.bottom });
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close(); sk->Release();
                shape = g;
            } else g->Release();
        }
    }
    D2D1_GRADIENT_STOP st[2] = { { 0, c0 }, { 1, c1 } };
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb))) {
        if (shape) rt->FillGeometry(shape, lb); else rt->FillRectangle(r, lb);
    }
    if (lb) lb->Release();
    if (gs) gs->Release();
    float dpx = 96, dpy = 96; rt->GetDpi(&dpx, &dpy);
    float px = 96.f / dpx;  // um pixel da tela, em DIP
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(G(hl), &b))) {
        // o fio de luz: no alto da barra de cima (abaixo do canto) e no alto da barra de baixo
        rt->FillRectangle(D2D1::RectF(r.left + (top ? rTop * 0.7f : 0), r.top + (top ? 0 : px), r.right - (top ? rTop * 0.7f : 0), r.top + (top ? px : 2 * px)), b);
        b->SetColor(edge);
        if (top) rt->FillRectangle(D2D1::RectF(r.left, r.bottom - px, r.right, r.bottom), b);
        else rt->FillRectangle(D2D1::RectF(r.left, r.top, r.right, r.top + px), b);
        b->Release();
    }
    if (shape) shape->Release();
    if (f) f->Release();
}

// cor da barra lateral do Finder/Ajustes do Leopard (o azul-acinzentado do iTunes 7): na frente / atrás
inline D2D1_COLOR_F Sidebar(bool active, bool dark) {
    if (dark) return D2D1::ColorF(0.17f, 0.18f, 0.20f, 1);
    return active ? D2D1::ColorF(214 / 255.f, 221 / 255.f, 229 / 255.f, 1) : D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1);
}

}  // namespace leopard
