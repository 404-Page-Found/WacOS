// Canto contínuo da Apple ("squircle"), o mesmo em todo o WacOS: as janelas da WacOSWindow.dll (recorte, moldura,
// capas, faixa dos botões, prévia do encaixe) e as que o Finder e os Ajustes desenham (tahoe::Squircle).
// É o traçado do "corner smoothing" de 60% do iOS/macOS (o que o Figma usa para imitá-lo): no meio, um arco do MESMO
// raio (a quina fica tão redonda quanto o círculo: na diagonal, 0,414 × raio da quina); nas pontas, duas curvas de
// Bézier que levam a curva até 1,6 × o raio e entram na borda reta sem o "degrau" do círculo.
// (21/11/2026. Antes: na DLL, arco de círculo; no Finder, superelipse n = 4 em 1,7 × raio, 0,382 × raio na diagonal.)
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

struct SqPerfil {
    float R = 0, p = 0;         // raio; até onde a curva vai a partir da quina (1,6 × R)
    std::vector<float> xs, ys;  // a curva do canto de cima à esquerda, quina em (0,0): de (p, 0) até (0, p)
    float Yat(float dx) const {  // altura da curva na distância dx da borda esquerda
        if (dx >= p) return 0;
        for (size_t i = 0; i + 1 < xs.size(); i++)
            if (dx <= xs[i] && dx >= xs[i + 1]) { float t = xs[i] == xs[i + 1] ? 0 : (xs[i] - dx) / (xs[i] - xs[i + 1]); return ys[i] + t * (ys[i + 1] - ys[i]); }
        return p;
    }
    float Dist(float x, float y) const {  // distância até a curva
        float best = 1e9f;
        for (size_t i = 0; i + 1 < xs.size(); i++) {
            float ax = xs[i], ay = ys[i], bx = xs[i + 1] - ax, by = ys[i + 1] - ay, l2 = bx * bx + by * by;
            float t = l2 > 0 ? std::clamp(((x - ax) * bx + (y - ay) * by) / l2, 0.f, 1.f) : 0.f;
            float ex = x - (ax + t * bx), ey = y - (ay + t * by);
            best = std::min(best, ex * ex + ey * ey);
        }
        return std::sqrt(best);
    }
};

inline const SqPerfil& PerfilSq(float R) {
    thread_local std::map<int, SqPerfil> cache;  // por raio (1/8 de px)
    int key = (int)std::lround(std::max(R, 0.f) * 8);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    SqPerfil& P = cache[key];
    R = key / 8.f;
    P.R = R;
    const float sm = 0.6f, deg = 3.14159265f / 180;
    float p = (1 + sm) * R;
    float arcMeasure = 90 * (1 - sm), arcLen = std::sin(arcMeasure / 2 * deg) * R * 1.41421356f;
    float alpha = (90 - arcMeasure) / 2, p34 = R * std::tan(alpha / 2 * deg), beta = 45 * sm;
    float c = p34 * std::cos(beta * deg), d = c * std::tan(beta * deg);
    float b = (p - arcLen - c - d) / 3, a = 2 * b;
    P.p = p;
    if (R <= 0) { P.xs = { 0, 0 }; P.ys = { 0, 0 }; return P; }
    // o traçado do canto de cima à DIREITA, a partir de (0,0) na borda de cima; vira o de cima à esquerda espelhando x
    auto add = [&](float X, float Y) { P.xs.push_back(p - X); P.ys.push_back(Y); };
    auto bez = [&](float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, bool first) {
        for (int k = first ? 0 : 1; k <= 16; k++) {
            float t = k / 16.f, u = 1 - t;
            add(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
        }
    };
    float p3x = a + b + c, p3y = d, p4x = p3x + arcLen, p4y = p3y + arcLen;
    bez(0, 0, a, 0, a + b, 0, p3x, p3y, true);
    float cx = p - R, cy = R, a0 = std::atan2(p3y - cy, p3x - cx), a1 = std::atan2(p4y - cy, p4x - cx);
    for (int k = 1; k <= 10; k++) { float an = a0 + (a1 - a0) * k / 10; add(cx + R * std::cos(an), cy + R * std::sin(an)); }
    bez(p4x, p4y, p4x + d, p4y + c, p4x + d, p4y + b + c, p4x + d, p4y + a + b + c, false);
    return P;
}

// o raio que cabe num retângulo de meias medidas hw × hh (a curva vai até 1,6 × raio)
inline float SqRaio(float r, float hw, float hh) { return std::max(0.f, std::min(r, std::min(hw, hh) / 1.6f)); }

// o retângulo inteiro com os quatro cantos contínuos, como figura fechada (D2D)
inline void SqRetangulo(ID2D1GeometrySink* sk, D2D1_RECT_F r, float radius) {
    float R = SqRaio(radius, (r.right - r.left) / 2, (r.bottom - r.top) / 2);
    const SqPerfil& P = PerfilSq(R);
    float p = R > 0 ? P.p : 0;
    size_t n = P.xs.size();
    // cada canto: os pontos (dx, dy) da curva, quina no canto do retângulo, no sentido horário
    sk->BeginFigure({ r.left + p, r.top }, D2D1_FIGURE_BEGIN_FILLED);
    sk->AddLine({ r.right - p, r.top });
    for (size_t i = 0; R > 0 && i < n; i++) sk->AddLine({ r.right - P.xs[i], r.top + P.ys[i] });            // cima à direita
    sk->AddLine({ r.right, r.bottom - p });
    for (size_t i = 0; R > 0 && i < n; i++) sk->AddLine({ r.right - P.ys[i], r.bottom - P.xs[i] });         // baixo à direita
    sk->AddLine({ r.left + p, r.bottom });
    for (size_t i = 0; R > 0 && i < n; i++) sk->AddLine({ r.left + P.xs[i], r.bottom - P.ys[i] });          // baixo à esquerda
    sk->AddLine({ r.left, r.top + p });
    for (size_t i = 0; R > 0 && i < n; i++) sk->AddLine({ r.left + P.ys[i], r.top + P.xs[i] });             // cima à esquerda
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
}
