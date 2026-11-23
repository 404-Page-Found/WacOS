// Siri (issue #39, 23/11/2026): pergunta pelo Spotlight ("Perguntar à Siri") e a resposta num painel de vidro no alto
// da tela, como a Siri do macOS. Não há servidor do WacOS: a pergunta vai direto do PC para o serviço que o usuário
// escolheu nos Ajustes (Gemini, Groq, OpenAI ou o Ollama local), com a chave dele (src/comum/siri.h).
// A chamada roda numa thread (WinHTTP, até 60 s); cada pergunta tem um número, e a resposta de uma pergunta antiga
// (painel fechado ou outra pergunta feita) é descartada. Esc ou clicar fora fecha; Ctrl+C copia a resposta.
#include "shell.h"
#include "../comum/siri.h"
#include "../comum/simbolos.h"
#include "../comum/idioma.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace {

const UINT WM_SIRI_FIM = WM_APP + 70, WM_SIRI_FRAME = WM_APP + 71;
const UINT_PTR T_PONTOS = 1;
const float W = 560, PAD = 20, ORB = 30, R = 22;
const float M = 28;  // folga em volta do painel para o brilho (os cliques nela atravessam: WM_NCHITTEST)

HWND g_wnd, g_back;
ui::GpuSurface g_s;
bool g_glass, g_inGlass, g_open;
std::wstring g_q, g_a;
bool g_busy, g_erro;
std::atomic<int> g_num{ 0 };
int g_pontos;
float g_scroll, g_contentH, g_viewH;
double g_openAt, g_answerAt, g_closeAt;
bool g_closing;
ID2D1StrokeStyle* g_round;
IDWriteTextFormat *g_tfQ, *g_tfA;

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

// o modelo não existe mais (ou nunca existiu) para esta conta: os serviços dizem isso de jeitos diferentes
bool ModeloSumiu(DWORD status, const std::wstring& msg) {
    if (status != 400 && status != 404) return false;
    std::wstring l = msg; for (auto& c : l) c = (wchar_t)towlower(c);
    for (const wchar_t* w : { L"no longer available", L"not found", L"does not exist", L"not supported", L"decommissioned", L"deprecated",
                              L"invalid model", L"unknown model", L"model_not_found", L"is not a valid model" })
        if (l.find(w) != std::wstring::npos) return true;
    return false;
}

std::wstring PerguntaCli(const siri::Provedor& pv, const std::wstring& q, bool& erro) {
    const std::wstring instr = L"Answer briefly and clearly, in the same language as the question, in plain text without Markdown. "
                               L"Do not run commands or edit files. Question: ";
    DWORD codigo = 0; bool tempo = false;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring ultima = std::wstring(tmp) + L"WacOS-Siri\\ultima.txt";
    DeleteFileW(ultima.c_str());
    std::wstring r = siri::RodaCli(pv.caminho, instr + q, codigo, tempo);
    Log(L"siri: %ls -> código %lu, %zu caracteres%ls", pv.nome, codigo, r.size(), tempo ? L" (passou do tempo)" : L"");
    if (tempo) return Tr(L"O app demorou demais para responder.");
    if (codigo == 9009 || codigo == ERROR_FILE_NOT_FOUND)  // o cmd não achou o comando
        return std::wstring(Tr(L"Não achei o app neste PC. Instale e entre na sua conta nele:")) + L" " + pv.site;
    // o Codex grava a última mensagem num arquivo (a saída tem o resto da conversa)
    HANDLE h = CreateFileW(ultima.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(h, nullptr), n = 0;
        std::string b(sz < (1u << 20) ? sz : 0, 0);
        if (!b.empty() && ReadFile(h, b.data(), sz, &n, nullptr)) r = siri::Wide(b);
        CloseHandle(h);
        DeleteFileW(ultima.c_str());
    }
    if (codigo != 0 || r.empty()) {
        std::wstring t = Limpa(r);
        if (t.size() > 400) t = t.substr(t.size() - 400);
        return std::wstring(Tr(L"O app não respondeu. Ele está instalado e com a conta entrada?")) + (t.empty() ? L"" : L"\n\n" + t);
    }
    erro = false;
    return Limpa(r);
}

std::wstring Pergunta(const std::wstring& q, bool& erro) {
    erro = true;
    const auto& provs = siri::Provedores();
    int p = std::clamp(_wtoi(shell::Setting(L"siri_provedor", L"0").c_str()), 0, (int)provs.size() - 1);
    if (p == 0) return Tr(L"Escolha um serviço para a Siri em Ajustes do Sistema > Apple Intelligence e Siri.");
    const auto& pv = provs[p];
    if (pv.fmt == siri::F_CLI) return PerguntaCli(pv, q, erro);
    std::wstring modelo = shell::Setting(L"siri_modelo", pv.modelos.empty() ? L"" : pv.modelos[0]);
    std::wstring chave = pv.precisaChave ? siri::LeChave(p) : L"";
    if (pv.precisaChave && chave.empty()) return Tr(L"Falta a chave da API. Cole a chave em Ajustes do Sistema > Apple Intelligence e Siri.");
    if (modelo.empty()) {  // LM Studio e afins: o modelo carregado
        std::wstring e; auto l = siri::ListaModelos(p, e);
        if (!l.empty()) modelo = l[0];
    }
    const std::wstring sistema = L"You are Siri inside WacOS, a macOS-style desktop for Windows. Answer briefly and clearly, "
                                 L"in the same language as the question. Use plain text, without Markdown.";
    std::wstring cab = siri::Cabecalhos(pv, chave);
    SecureZeroMemory(chave.data(), chave.size() * sizeof(wchar_t));
    siri::Resposta r;
    for (int tentativa = 0; tentativa < 3; tentativa++) {
        std::string body;
        std::wstring caminho = pv.caminho;
        if (pv.fmt == siri::F_GEMINI) {
            size_t m = caminho.find(L"{m}"); if (m != std::wstring::npos) caminho.replace(m, 3, modelo);
            body = "{\"systemInstruction\":{\"parts\":[{\"text\":" + siri::JStr(sistema) + "}]},\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":" + siri::JStr(q) + "}]}]}";
        } else if (pv.fmt == siri::F_ANTHROPIC) {
            body = "{\"model\":" + siri::JStr(modelo) + ",\"max_tokens\":1024,\"system\":" + siri::JStr(sistema) +
                   ",\"messages\":[{\"role\":\"user\",\"content\":" + siri::JStr(q) + "}]}";
        } else {  // OpenAI e compatíveis (Groq, OpenRouter, Mistral, DeepSeek, xAI, LM Studio) e Ollama
            body = "{\"model\":" + siri::JStr(modelo) + ",\"messages\":[{\"role\":\"system\",\"content\":" + siri::JStr(sistema) + "},{\"role\":\"user\",\"content\":" + siri::JStr(q) + "}]" +
                   (pv.fmt == siri::F_OLLAMA ? ",\"stream\":false}" : "}");
        }
        r = siri::Http(pv, L"POST", caminho, cab, body);
        Log(L"siri: %ls %ls -> %lu (%zu bytes, erro %lu)", pv.nome, modelo.c_str(), r.status, r.corpo.size(), r.falha);
        if (r.falha || tentativa >= 2) break;
        if (r.status == 503 || r.status == 500 || r.status == 529) {  // sobrecarregado: de novo; depois, outro modelo da lista (só nesta pergunta)
            if (!tentativa) { Sleep(1500); continue; }
            std::wstring e; auto lista = siri::ListaModelos(p, e);
            std::wstring alt;
            // o reserva: estável (sem preview/omni/exp), de preferência da mesma família (Flash no Gemini)
            for (auto& m : lista) {
                if (m == modelo || m.find(L"lite") != std::wstring::npos || m.find(L"preview") != std::wstring::npos ||
                    m.find(L"omni") != std::wstring::npos || m.find(L"exp") != std::wstring::npos || m.find(L"latest") != std::wstring::npos) continue;
                bool familia = pv.fmt != siri::F_GEMINI || m.find(L"flash") != std::wstring::npos;
                if (familia) { alt = m; break; }
                if (alt.empty()) alt = m;
            }
            if (!alt.empty()) { Log(L"siri: %ls sobrecarregado; tentando %ls", modelo.c_str(), alt.c_str()); modelo = alt; continue; }
            break;
        }
        // cota zerada deste modelo (o plano grátis não inclui todos): tenta o preferido da lista, que costuma estar no plano
        std::wstring msgErro = siri::ErroDoServico(r.corpo), lm = msgErro;
        for (auto& c : lm) c = (wchar_t)towlower(c);
        bool semCota = r.status == 429 && lm.find(L"quota") != std::wstring::npos;
        if (semCota) {
            std::wstring e; auto lista = siri::ListaModelos(p, e);
            if (!lista.empty() && lista[0] != modelo) {
                Log(L"siri: sem cota em %ls; tentando %ls", modelo.c_str(), lista[0].c_str());
                modelo = lista[0];
                shell::SetSetting(L"siri_modelo", modelo.c_str());
                continue;
            }
        }
        if (!ModeloSumiu(r.status, msgErro)) { if (r.status >= 400) Log(L"siri: erro do serviço: %ls", siri::ErroDoServico(r.corpo).substr(0, 200).c_str()); break; }
        // o modelo saiu do ar: pega a lista atual do serviço, guarda o preferido dela e pergunta de novo
        std::wstring e; auto lista = siri::ListaModelos(p, e);
        if (lista.empty() || lista[0] == modelo) { Log(L"siri: o modelo saiu do ar, mas a lista não trouxe outro (%ls)", e.empty() ? L"vazia" : e.c_str()); break; }
        Log(L"siri: %ls não existe mais; agora %ls", modelo.c_str(), lista[0].c_str());
        modelo = lista[0];
        shell::SetSetting(L"siri_modelo", modelo.c_str());
        std::wstring junta; for (auto& m : lista) junta += (junta.empty() ? L"" : L"|") + m;
        shell::SetSetting((L"siri_modelos_" + std::to_wstring(p)).c_str(), junta.c_str());
    }
    SecureZeroMemory(cab.data(), cab.size() * sizeof(wchar_t));  // a chave ia no cabeçalho
    if (r.falha) {
        if (pv.porta != 443) return std::wstring(Tr(L"Não deu para falar com o app neste PC. Ele está aberto?")) + L" (" + pv.nome + L")";
        return Tr(L"Não deu para falar com o serviço. Confira a internet.");
    }
    std::wstring t;
    if (r.status >= 200 && r.status < 300) {
        bool ok = pv.fmt == siri::F_GEMINI ? siri::JLe(r.corpo, "text", r.corpo.find("\"candidates\""), t)
                : pv.fmt == siri::F_ANTHROPIC ? siri::JLe(r.corpo, "text", r.corpo.find("\"content\""), t)
                : siri::JLe(r.corpo, "content", r.corpo.find("\"message\""), t);
        if (ok && !t.empty()) { erro = false; return Limpa(t); }
        return Tr(L"O serviço respondeu sem texto.");
    }
    t = siri::ErroDoServico(r.corpo);
    if (r.status == 429) {
        std::wstring l = t; for (auto& c : l) c = (wchar_t)towlower(c);
        if (l.find(L"quota") != std::wstring::npos || l.find(L"rate") != std::wstring::npos)
            return std::wstring(Tr(L"Acabou a cota deste modelo no seu plano (o grátis tem limite por minuto e por dia). Espere um pouco ou escolha outro modelo em Ajustes do Sistema > Apple Intelligence e Siri.")) + L" (" + modelo + L")";
    }
    // as mensagens dos serviços podem ter várias linhas e links: só a primeira frase
    if (size_t nlPos = t.find(L'\n'); nlPos != std::wstring::npos) t = t.substr(0, nlPos);
    if (t.size() > 240) t = t.substr(0, 237) + L"...";
    if (!t.empty()) return (r.status == 401 || r.status == 403 ? std::wstring(Tr(L"Chave recusada pelo serviço.")) + L" " : std::wstring()) + t;
    return std::wstring(Tr(L"O serviço recusou a pergunta")) + L" (" + std::to_wstring(r.status) + L").";
}

// ---------------------------------------------------------------- painel
float Sc() { return g_wnd ? ui::Scale(g_wnd) : 1.f; }

float AlturaTexto(const std::wstring& t, IDWriteTextFormat* f, float w) {
    IDWriteTextLayout* l = nullptr; float h = 0;
    if (f && SUCCEEDED(ui::dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, w, 10000, &l))) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); h = m.height; l->Release(); }
    return h;
}

// ---- o brilho da Siri: uma luz colorida que corre pela borda do painel e se espalha para fora, como a do Apple
// Intelligence. A cor de cada ponto da borda depende da posição nela e do tempo (as cores andam em volta); por baixo, três
// traços largos e fracos fazem o brilho; por cima, o fio forte. Enquanto pensa: inteiro e "respirando"; quando a resposta
// chega, apaga em 0,9 s.
D2D1_COLOR_F CorDaVolta(float u) {  // u em [0,1): rosa, roxo, azul, ciano, laranja e de volta ao rosa
    static const D2D1_COLOR_F c[] = { { 1.f, 0.33f, 0.62f, 1 }, { 0.68f, 0.36f, 1.f, 1 }, { 0.26f, 0.52f, 1.f, 1 },
                                      { 0.30f, 0.86f, 1.f, 1 }, { 1.f, 0.64f, 0.30f, 1 } };
    const int n = 5;
    u = u - std::floor(u);
    float f = u * n; int a = (int)f % n, b = (a + 1) % n; float k = f - std::floor(f);
    k = k * k * (3 - 2 * k);  // suave entre as cores
    return { c[a].r + (c[b].r - c[a].r) * k, c[a].g + (c[b].g - c[a].g) * k, c[a].b + (c[b].b - c[a].b) * k, 1 };
}
// ponto da borda do retângulo arredondado em u (0..1, sentido horário a partir do meio de cima)
D2D1_POINT_2F PontoDaBorda(const D2D1_RECT_F& r, float rad, float u) {
    float w = r.right - r.left, h = r.bottom - r.top, arco = 1.5707963f * rad;
    float retoX = w - 2 * rad, retoY = h - 2 * rad, total = 2 * retoX + 2 * retoY + 4 * arco;
    float d = (u - std::floor(u)) * total;
    auto canto = [&](float cx, float cy, float a0, float t) { float a = a0 + t / rad; return D2D1_POINT_2F{ cx + std::cos(a) * rad, cy + std::sin(a) * rad }; };
    float x0 = r.left + w / 2;
    // do meio de cima para a direita
    float seg = retoX / 2; if (d < seg) return { x0 + d, r.top }; d -= seg;
    if (d < arco) return canto(r.right - rad, r.top + rad, -1.5707963f, d); d -= arco;
    if (d < retoY) return { r.right, r.top + rad + d }; d -= retoY;
    if (d < arco) return canto(r.right - rad, r.bottom - rad, 0, d); d -= arco;
    if (d < retoX) return { r.right - rad - d, r.bottom }; d -= retoX;
    if (d < arco) return canto(r.left + rad, r.bottom - rad, 1.5707963f, d); d -= arco;
    if (d < retoY) return { r.left, r.bottom - rad - d }; d -= retoY;
    if (d < arco) return canto(r.left + rad, r.top + rad, 3.1415926f, d); d -= arco;
    return { r.left + rad + std::min(d, retoX / 2), r.top };
}
void Brilho(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, float rad, double tMs, float forca) {
    if (forca <= 0.01f) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (!br) return;
    const int N = 180;
    float gira = (float)(tMs / 2600.0);  // as cores dão uma volta a cada 2,6 s
    struct Camada { float largura, alfa; } camadas[] = { { 22, 0.06f }, { 13, 0.10f }, { 7, 0.18f }, { 2.6f, 0.95f } };
    std::vector<D2D1_POINT_2F> pts(N + 1);
    for (int k = 0; k <= N; k++) pts[k] = PontoDaBorda(box, rad, (float)k / N);
    for (auto& cm : camadas) {
        for (int k = 0; k < N; k++) {
            float u = (float)k / N;
            D2D1_COLOR_F c = CorDaVolta(u * 2 + gira);  // duas voltas de cor pela borda inteira
            // um trecho mais claro corre pela borda (a "onda" de luz)
            float onda = 0.55f + 0.45f * std::cos(6.2831853f * (u - gira * 0.7f));
            c.a = cm.alfa * forca * (cm.largura > 3 ? onda : 0.75f + 0.25f * onda);
            br->SetColor(c);
            dc->DrawLine(pts[k], pts[k + 1], br, cm.largura, g_round);
        }
    }
    br->Release();
}

void Orbe(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float r, double t) {
    // a bolinha da Siri: as cores giram enquanto pensa (e devagar depois), com um brilho que pulsa
    D2D1_GRADIENT_STOP gs[4] = { { 0, D2D1::ColorF(0.98f, 0.36f, 0.62f) }, { 0.35f, D2D1::ColorF(0.62f, 0.38f, 1.f) },
                                 { 0.7f, D2D1::ColorF(0.25f, 0.62f, 1.f) }, { 1, D2D1::ColorF(1.f, 0.62f, 0.32f) } };
    ID2D1GradientStopCollection* sc = nullptr;
    if (FAILED(dc->CreateGradientStopCollection(gs, 4, &sc))) return;
    float a = (float)(t / (g_busy ? 700.0 : 2400.0));
    float pulso = g_busy ? 1.f + 0.07f * (float)std::sin(t / 260.0) : 1.f;
    float rr = r * pulso;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.38f, 1.f, g_busy ? 0.22f : 0.12f), &br);
    if (br) { dc->FillEllipse(D2D1::Ellipse(c, rr + 5, rr + 5), br); }  // halo
    D2D1_POINT_2F p0{ c.x + std::cos(a) * rr, c.y + std::sin(a) * rr }, p1{ c.x - std::cos(a) * rr, c.y - std::sin(a) * rr };
    ID2D1LinearGradientBrush* lg = nullptr;
    if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), sc, &lg))) {
        dc->FillEllipse(D2D1::Ellipse(c, rr, rr), lg);
        lg->Release();
    }
    sc->Release();
    if (br) {  // reflexo em cima, como uma bolha de vidro
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.38f));
        dc->FillEllipse(D2D1::Ellipse({ c.x - rr * 0.25f, c.y - rr * 0.38f }, rr * 0.45f, rr * 0.25f), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        dc->DrawEllipse(D2D1::Ellipse(c, rr - 0.5f, rr - 0.5f), br, 1);
        br->Release();
    }
}

// quanto do brilho aparece agora (1 pensando, apagando em 0,9 s depois da resposta)
float ForcaBrilho(double now) {
    if (g_busy) return 0.85f + 0.15f * (float)std::sin(now / 420.0);
    float k = (float)((now - g_answerAt) / 900.0);
    return k >= 1 ? 0.f : (1 - k) * (1 - k);
}
bool Animando(double now) {
    return g_closing || g_busy || !anim::PanelIn(now - g_openAt).done || now - g_answerAt < 950;
}

void Paint() {
    if ((!g_open && !g_closing) || !g_wnd) return;
    double now = anim::Now();
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
    int wpx = (int)std::lround((W + 2 * M) * sc), hpx = (int)std::lround((H + 2 * M) * sc);
    RECT wr; GetWindowRect(g_wnd, &wr);
    if (wr.bottom - wr.top != hpx || wr.right - wr.left != wpx) {
        SetWindowPos(g_wnd, nullptr, 0, 0, wpx, hpx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_back) SetWindowPos(g_back, g_wnd, wr.left, wr.top, wpx, hpx, SWP_NOACTIVATE);
    }
    float mpx = M * sc;
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(mpx, mpx, wpx - mpx, hpx - mpx), R * sc } },
                                     dark ? D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.55f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.50f), 24.f);
    ID2D1DeviceContext* dc = g_glass ? backdrop::ContentBegin(g_back, wpx, hpx, sc) : nullptr;
    g_inGlass = dc != nullptr;
    if (!dc) dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    D2D1_MATRIX_3X2_F base; dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(M, M) * base);  // o painel começa na folga
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    if (br) {
        D2D1_RECT_F box = D2D1::RectF(0, 0, W, H);
        Brilho(dc, D2D1::RectF(-1, -1, W + 1, H + 1), R + 1, now - g_openAt, ForcaBrilho(now));
        if (!g_glass) { br->SetColor(dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.95f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.95f)); dc->FillRoundedRectangle(D2D1::RoundedRect(box, R, R), br); }
        if (liquid::On()) liquid::Rim(dc, box, R, dark);
        else { br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br, 1); }
        Orbe(dc, { PAD + ORB / 2, PAD + ORB / 2 }, ORB / 2, now - g_openAt);
        float x = PAD + ORB + 14;
        dc->PushAxisAlignedClip(D2D1::RectF(x - 2, 8, W - PAD + 2, H - 8), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = PAD - g_scroll;
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f));
        dc->DrawTextW(g_q.c_str(), (UINT32)g_q.size(), g_tfQ, D2D1::RectF(x, y, x + textW, y + qh + 4), br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        y += qh + 8;
        // a resposta entra subindo um pouco e aparecendo (0,35 s)
        float k = g_busy ? 1.f : std::clamp((float)((now - g_answerAt) / 350.0), 0.f, 1.f);
        float e = 1 - (1 - k) * (1 - k) * (1 - k);
        D2D1_COLOR_F tc = g_erro ? D2D1::ColorF(1.f, 0.42f, 0.38f) : dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.88f);
        tc.a *= g_busy ? 0.6f + 0.25f * (float)std::sin(now / 300.0) : e;
        br->SetColor(tc);
        float dy = (1 - e) * 8;
        dc->DrawTextW(a.c_str(), (UINT32)a.size(), g_tfA, D2D1::RectF(x, y + dy, x + textW, y + dy + ah + 4), br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);  // emoji colorido
        dc->PopAxisAlignedClip();
        if (g_contentH > H) {  // barrinha de rolagem fina
            float bh = std::max(24.f, H * H / g_contentH), by = 6 + (H - 12 - bh) * (g_scroll / (g_contentH - H));
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.3f) : D2D1::ColorF(0, 0, 0, 0.25f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(W - 8, by, W - 4, by + bh), 2, 2), br);
        }
        br->Release();
    }
    dc->SetTransform(base);
    if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s);
}

// a mola de abrir (crescendo do meio) e a de fechar: escala e opacidade pelo compositor
void Movimento() {
    double now = anim::Now();
    anim::Panel mo = g_closing ? anim::PanelOut(now - g_closeAt) : anim::PanelIn(now - g_openAt);
    RECT wr; GetWindowRect(g_wnd, &wr);
    float cx = (wr.right - wr.left) / 2.f, cy = (wr.bottom - wr.top) / 2.f;
    if (g_inGlass) backdrop::Fade(g_back, mo.fade, mo.scale, cx, cy);
    else { ui::GpuMotion(g_s, mo.scale, cx, cy, mo.fade); if (g_back) backdrop::Fade(g_back, mo.fade, mo.scale, cx, cy); }
}

void Esconde() {
    Log(L"siri: painel escondido");
    g_closing = false;
    ui::FrameClock(g_wnd, WM_SIRI_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    if (g_back) ShowWindow(g_back, SW_HIDE);
}

void Fecha() {
    if (!g_open) return;
    g_open = false; g_busy = false;
    g_num++;  // a resposta que ainda vem é descartada
    KillTimer(g_wnd, T_PONTOS);
    if (anim::reduceMotion) { Esconde(); return; }
    g_closing = true; g_closeAt = anim::Now();
    ui::FrameClock(g_wnd, WM_SIRI_FRAME, true);
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
            g_answerAt = anim::Now();
            KillTimer(h, T_PONTOS);
            Paint();  // o brilho apaga e a resposta entra: os quadros continuam (WM_SIRI_FRAME)
        }
        delete r;
        return 0;
    }
    case WM_SIRI_FRAME: {
        ui::FrameAck(h, WM_SIRI_FRAME);
        double now = anim::Now();
        if (g_closing) {
            Movimento();
            if (anim::PanelOut(now - g_closeAt).done) Esconde();
            return 0;
        }
        Paint(); Movimento();
        if (!Animando(now)) ui::FrameClock(h, WM_SIRI_FRAME, false);
        return 0;
    }
    case WM_NCHITTEST: {  // só o painel pega o clique; a folga do brilho deixa passar
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT wr; GetWindowRect(h, &wr);
        float sc = Sc(); int mpx = (int)std::lround(M * sc);
        return pt.x >= wr.left + mpx && pt.x < wr.right - mpx && pt.y >= wr.top + mpx && pt.y < wr.bottom - mpx ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_TIMER: if (w == T_PONTOS) { g_pontos++; if (!Animando(anim::Now())) Paint(); } return 0;
    case WM_MOUSEWHEEL: g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * 48; Paint(); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Fecha();
        else if (w == 'C' && GetKeyState(VK_CONTROL) < 0) Copia();
        else if (w == VK_DOWN) { g_scroll += 40; Paint(); }
        else if (w == VK_UP) { g_scroll -= 40; Paint(); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_open) {
            wchar_t c[64] = {}; if (l) GetClassNameW((HWND)l, c, 64);
            Log(L"siri: perdeu a frente para %ls; fecha", c[0] ? c : L"(nenhuma)");
            Fecha();
        }
        return 0;
    case WM_CLOSE: Fecha(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
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
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    ui::d2d->CreateStrokeStyle(sp, nullptr, 0, &g_round);
    return true;
}

}  // namespace

bool siri::Pronta() {
    const auto& provs = siri::Provedores();
    int p = _wtoi(shell::Setting(L"siri_provedor", L"0").c_str());
    if (p <= 0 || p >= (int)provs.size()) return false;
    return !provs[p].precisaChave || GetFileAttributesW(siri::ArquivoChave(p).c_str()) != INVALID_FILE_ATTRIBUTES;
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
    int wpx = (int)std::lround((W + 2 * M) * sc), hpx = (int)std::lround((ORB + PAD * 2 + 2 * M) * sc);
    int x = (mi.rcWork.left + mi.rcWork.right - wpx) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 6 - (int)std::lround(M * sc);
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    if (g_back) SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE);
    g_open = true; g_closing = false; g_openAt = anim::Now(); g_answerAt = 0;
    if (g_glass) backdrop::Fade(g_back, 0, 0.94f, wpx / 2.f, hpx / 2.f);  // nasce invisível e um pouco menor: a mola leva até o tamanho
    else ui::GpuMotion(g_s, 0.94f, wpx / 2.f, hpx / 2.f, 0);
    Paint();
    if (g_back) ShowWindow(g_back, SW_SHOWNOACTIVATE);
    ShowWindow(g_wnd, SW_SHOW);
    shell::LiberaFrente();
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    SetTimer(g_wnd, T_PONTOS, 420, nullptr);
    ui::FrameClock(g_wnd, WM_SIRI_FRAME, true);  // a mola de abrir, o brilho e a bolinha enquanto pensa
    HWND to = g_wnd;
    std::thread([q, num, to] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool erro = false;
        std::wstring r = Pergunta(q, erro);
        PostMessageW(to, WM_SIRI_FIM, (WPARAM)num, (LPARAM)new std::wstring((erro ? L"\x1" : L"") + r));
        CoUninitialize();
    }).detach();
}
