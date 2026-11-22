// SF Symbols (SVGs exportados da fonte SF Pro, em icones\simbolos) desenhados com Direct2D.
// Os SVGs têm um único caminho com M, L, Q e Z e o eixo Y invertido; viram uma ID2D1PathGeometry
// (com cache) e são preenchidos na cor do pincel, centralizados na caixa pedida.
#pragma once
#include <d2d1.h>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>

namespace simbolo {

struct Sym { ID2D1PathGeometry* geo = nullptr; float w = 0, h = 0; };

inline std::wstring& Dir() { static std::wstring d; return d; }  // pasta com os .svg

// O texto de cada .svg, lido do disco uma vez. Com Preload (o Shell chama ao abrir), todos vêm numa thread: lido na
// hora de desenhar, o fread parava a interface do Shell por 1,7 s com o disco ocupado (pilha de 22/11/2026:
// Spotlight -> DrawIcon -> simbolo::Load -> fread). Sem Preload (os apps), lê na hora, como antes.
struct Textos { std::mutex mx; std::map<std::wstring, std::string> svg; std::atomic<int> estado{ 0 }; };  // 0 nada, 1 lendo, 2 pronto
inline Textos& Txt() { static Textos t; return t; }
inline bool LeArquivo(const std::wstring& path, std::string& t) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) t.append(buf, n);
    fclose(fp);
    return true;
}
inline void Preload() {
    int nada = 0;
    if (Dir().empty() || !Txt().estado.compare_exchange_strong(nada, 1)) return;
    std::thread([dir = Dir()] {
        std::map<std::wstring, std::string> lidos;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.svg").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring nome = fd.cFileName;
                std::string t;
                if (LeArquivo(dir + L"\\" + nome, t)) lidos[nome.substr(0, nome.size() - 4)] = std::move(t);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        { std::lock_guard<std::mutex> l(Txt().mx); for (auto& [k, v] : lidos) Txt().svg.emplace(k, std::move(v)); }
        Txt().estado = 2;
    }).detach();
}

inline Sym* Load(ID2D1Factory* f, const std::wstring& name) {
    // uma forma por fábrica do Direct2D: a barra de menus (CPU) e as janelas na GPU têm fábricas diferentes,
    // e geometria de uma não pode ser usada na outra (D2DERR_WRONG_FACTORY)
    static std::map<std::pair<ID2D1Factory*, std::wstring>, Sym> cache;
    auto key = std::make_pair(f, name);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.geo ? &it->second : nullptr;
    if (Dir().empty()) return nullptr;
    std::string t;
    const int estado = Txt().estado;
    if (estado) {  // pré-carregando ou pronto: nada de disco aqui
        std::lock_guard<std::mutex> l(Txt().mx);
        auto tx = Txt().svg.find(name);
        if (tx != Txt().svg.end()) t = tx->second;
    }
    if (t.empty()) {
        if (estado == 1) return nullptr;  // ainda lendo: o próximo desenho acha (sem guardar "não existe")
        if (estado == 0 && !LeArquivo(Dir() + L"\\" + name + L".svg", t)) { cache[key]; return nullptr; }
        if (estado == 2) { cache[key]; return nullptr; }  // não existe
    }
    Sym& s = cache[key];
    auto attr = [&](const char* key) -> float {
        size_t p = t.find(key);
        return p == std::string::npos ? 0.f : (float)atof(t.c_str() + p + strlen(key));
    };
    s.w = attr("width='");
    s.h = attr("height='");
    size_t d = t.find(" d='");
    if (d == std::string::npos || s.w <= 0 || s.h <= 0) return nullptr;
    size_t end = t.find('\'', d + 4);
    std::string path = t.substr(d + 4, end - d - 4);
    if (FAILED(f->CreatePathGeometry(&s.geo))) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    s.geo->Open(&sink);
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);  // fill-rule nonzero
    const char* p = path.c_str();
    auto num = [&]() -> float {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        char* e = nullptr;
        float v = strtof(p, &e);
        p = e;
        return v;
    };
    auto pt = [&]() { float x = num(); float y = num(); return D2D1::Point2F(x, s.h - y); };  // Y invertido
    bool open = false;
    while (*p) {
        char c = *p;
        if (c == 'M') { p++; if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->BeginFigure(pt(), D2D1_FIGURE_BEGIN_FILLED); open = true; }
        else if (c == 'L') { p++; sink->AddLine(pt()); }
        else if (c == 'Q') { p++; D2D1_POINT_2F a = pt(), b = pt(); sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(a, b)); }
        else if (c == 'Z') { p++; if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
        else p++;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return &s;
}

// Desenha o símbolo centralizado em `box` (mantendo a proporção). false se o .svg não existir.
inline bool Draw(ID2D1RenderTarget* rt, const std::wstring& name, D2D1_RECT_F box, ID2D1Brush* b) {
    if (name.empty()) return false;
    ID2D1Factory* f = nullptr;
    rt->GetFactory(&f);
    Sym* s = Load(f, name);
    f->Release();
    if (!s) return false;
    // encaixa o contorno real do símbolo (o SVG tem folga em volta, diferente em cada um)
    D2D1_RECT_F g{};
    s->geo->GetBounds(nullptr, &g);
    float gw = std::max(g.right - g.left, 1.f), gh = std::max(g.bottom - g.top, 1.f);
    float bw = box.right - box.left, bh = box.bottom - box.top;
    float k = std::min(bw / gw, bh / gh);
    float x = box.left + (bw - gw * k) / 2 - g.left * k, y = box.top + (bh - gh * k) / 2 - g.top * k;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y) * old);
    rt->FillGeometry(s->geo, b);
    rt->DrawGeometry(s->geo, b, 1.0f);  // o SVG tem contorno de 1 unidade: mesmo peso do original
    rt->SetTransform(old);
    return true;
}

}  // namespace simbolo
