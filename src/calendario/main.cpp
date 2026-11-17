// Calendário (Calendar do Mac): Dia, Semana e Mês (Ctrl+1/2/3), "‹ Hoje ›" no alto à direita, o mês grande à
// esquerda. Duplo clique cria um evento (no Mês: o dia inteiro; na Semana/Dia: uma hora a partir dali) e o nome
// já fica para digitar. Botão direito no evento: horário, duração, cor, apagar. Ctrl+I importa um .ics (Google,
// Outlook, iCloud exportam). Os lembretes com data (app Lembretes) aparecem como itens com quadradinho.
// Dados: %APPDATA%\Shell\calendario.txt. O Shell avisa (banner do Mac) quando o evento começa.
#include "../comum/app2d.h"
#include <shobjidl.h>
#include <map>

using namespace app2d;

namespace {

const float TB = 64, WEEKHEAD = 30, HOUR = 48, ALLDAY = 26, LEFTCOL = 56;
const UINT WM_FIELDLOST = WM_APP + 77;

struct Ev { int id; long long start, end; bool allDay; std::wstring title; int color; std::wstring notes; int cal = 1; };
std::vector<Ev> g_ev;
// calendários (barra lateral): cada evento é de um; esconder um calendário esconde os eventos dele
struct Cal { int id; std::wstring name; int color; bool on; };
std::vector<Cal> g_cals;
bool g_side = true;
const float SIDE = 200;
Cal* FindCal(int id) { for (auto& c : g_cals) if (c.id == id) return &c; return nullptr; }
bool CalOn(const Ev& e) { Cal* c = FindCal(e.cal); return !c || c->on; }
int g_nextId = 1;
std::wstring g_file, g_remFile;
enum View { V_DAY, V_WEEK, V_MONTH, V_YEAR };
View g_view = V_MONTH;
long long g_anchor;  // um dia (minutos) dentro do período mostrado
long long g_selDay;  // dia escolhido
int g_selEv = -1;
float g_scrollH = 8 * 48;  // Semana/Dia: rolagem (começa às 8h)
int g_editing = -1;
IDWriteTextFormat *g_tfYearDay, *g_tfMonth, *g_tfYear, *g_tfBtn, *g_tfDay, *g_tfDayNum, *g_tfEv, *g_tfEvSmall, *g_tfHour, *g_tfHeadDay, *g_tfIcon;

D2D1_COLOR_F EvColor(int i) {
    static const D2D1_COLOR_F c[] = { Rgb(10, 132, 255), Rgb(255, 69, 58), Rgb(255, 159, 10), Rgb(48, 209, 88), Rgb(191, 90, 242), Rgb(255, 214, 10), Rgb(172, 142, 104) };
    return c[((i % 7) + 7) % 7];
}
const wchar_t* COLOR_NAMES[] = { L"Azul", L"Vermelho", L"Laranja", L"Verde", L"Roxo", L"Amarelo", L"Marrom" };

void Save() {
    std::vector<std::wstring> out{ L"# Calendário do WacOS (E = evento: id, início, fim em minutos desde 1970, dia inteiro, título, cor, notas)" };
    for (auto& c : g_cals) out.push_back(L"C\t" + std::to_wstring(c.id) + L"\t" + Esc(c.name) + L"\t" + std::to_wstring(c.color) + L"\t" + (c.on ? L"1" : L"0"));
    for (auto& e : g_ev) out.push_back(L"E\t" + std::to_wstring(e.id) + L"\t" + std::to_wstring(e.start) + L"\t" + std::to_wstring(e.end) + L"\t" + (e.allDay ? L"1" : L"0") + L"\t" +
                                       Esc(e.title) + L"\t" + std::to_wstring(e.color) + L"\t" + Esc(e.notes) + L"\t" + std::to_wstring(e.cal));
    WriteLines(g_file, out);
}
void Load() {
    g_ev.clear(); g_cals.clear();
    for (auto& line : ReadLines(g_file)) {
        auto f = Split(line);
        if (f.size() >= 7 && f[0] == L"E") {
            Ev e{ _wtoi(f[1].c_str()), _wtoi64(f[2].c_str()), _wtoi64(f[3].c_str()), f[4] == L"1", Unesc(f[5]), _wtoi(f[6].c_str()), f.size() > 7 ? Unesc(f[7]) : L"" };
            if (f.size() > 8) e.cal = _wtoi(f[8].c_str());
            g_ev.push_back(e);
        } else if (f.size() >= 5 && f[0] == L"C") g_cals.push_back({ _wtoi(f[1].c_str()), Unesc(f[2]), _wtoi(f[3].c_str()), f[4] == L"1" });
    }
    if (g_cals.empty()) g_cals = { { 1, Tr(L"Pessoal"), 0, true }, { 2, Tr(L"Trabalho"), 2, true }, { 3, Tr(L"Família"), 3, true } };
    for (auto& e : g_ev) g_nextId = std::max(g_nextId, e.id + 1);
    for (auto& c : g_cals) g_nextId = std::max(g_nextId, c.id + 1);
}
Ev* FindEv(int id) { for (auto& e : g_ev) if (e.id == id) return &e; return nullptr; }

// lembretes com data (só leitura): "R id lista feito data título notas sinal"
struct RemItem { long long due; std::wstring title; bool done; };
std::vector<RemItem> Reminders() {
    std::vector<RemItem> v;
    for (auto& line : ReadLines(g_remFile)) {
        auto f = Split(line);
        if (f.size() >= 8 && f[0] == L"R" && _wtoi64(f[4].c_str())) v.push_back({ _wtoi64(f[4].c_str()), Unesc(f[5]), f[3] == L"1" });
    }
    return v;
}

long long AddDays(long long day, int n) { SYSTEMTIME s = FromMin(day); s.wHour = 12; long long m = ToMin(s) + n * 24LL * 60; return DayStart(m); }
long long MonthStart(long long m) { SYSTEMTIME s = FromMin(m); s.wDay = 1; s.wHour = s.wMinute = 0; return ToMin(s); }
long long AddMonths(long long m, int n) { SYSTEMTIME s = FromMin(MonthStart(m)); int mo = s.wMonth - 1 + n; s.wYear = (WORD)(s.wYear + (mo >= 0 ? mo / 12 : (mo - 11) / 12)); s.wMonth = (WORD)(((mo % 12) + 12) % 12 + 1); return ToMin(s); }
long long WeekStart(long long m) { long long d = DayStart(m); return AddDays(d, -(int)FromMin(d).wDayOfWeek); }  // domingo

bool OnDay(const Ev& e, long long day) { long long next = AddDays(day, 1); return e.start < next && (e.end > day || (e.end <= e.start && e.start >= day)); }

std::wstring Hm(long long m) { SYSTEMTIME s = FromMin(m); wchar_t b[16]; swprintf_s(b, L"%d:%02d", s.wHour, s.wMinute); return b; }

// ---- desenho
struct Hit { int kind, ev; long long day; D2D1_RECT_F r; };  // kind: 1 evento, 2 dia (célula/coluna), 3 botão, 4 visão
std::vector<Hit> g_hits;

void Toolbar(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray) {
    SYSTEMTIME a = FromMin(g_anchor);
    std::wstring mon = MonthName(a.wMonth);
    if (!idioma::En() && !mon.empty()) mon[0] = towupper(mon[0]);
    float mw = TextW(g_tfMonth, mon);
    Text(mon, g_tfMonth, D2D1::RectF(20, 12, 20 + mw + 4, 52), text);
    Text(std::to_wstring(a.wYear), g_tfYear, D2D1::RectF(26 + mw, 12, 26 + mw + 120, 52), gray);
    // Dia | Semana | Mês no meio
    const wchar_t* names[] = { L"Dia", L"Semana", L"Mês", L"Ano" };
    float segW = 70, sx = sz.width / 2 - segW * 2;
    Fill(D2D1::RectF(sx, 18, sx + segW * 4, 44), dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(sx + i * segW + 2, 20, sx + (i + 1) * segW - 2, 42);
        if (g_view == i) Fill(r, dark ? Rgb(99, 99, 102) : Rgb(255, 255, 255), 6);
        Text(Tr(names[i]), g_tfBtn, r, text);
        g_hits.push_back({ 4, i, 0, r });
    }
    // ‹ Hoje ›
    D2D1_RECT_F prev = D2D1::RectF(sz.width - 150, 18, sz.width - 122, 44), today = D2D1::RectF(sz.width - 120, 18, sz.width - 50, 44), next = D2D1::RectF(sz.width - 48, 18, sz.width - 20, 44);
    Fill(today, dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    Text(Tr(L"Hoje"), g_tfBtn, today, text);
    Text(L"‹", g_tfMonth, prev, text);
    Text(L"›", g_tfMonth, next, text);
    g_hits.push_back({ 3, -1, 0, prev }); g_hits.push_back({ 3, 0, 0, today }); g_hits.push_back({ 3, 1, 0, next });
}

void PaintMonth(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, const std::vector<RemItem>& rems) {
    long long ms = MonthStart(g_anchor), first = WeekStart(ms);
    SYSTEMTIME am = FromMin(ms);
    float top = TB + WEEKHEAD, cw = sz.width / 7, rows = 6, ch = (sz.height - top) / rows;
    for (int d = 0; d < 7; d++) Text(WeekdayShort(d), g_tfHeadDay, D2D1::RectF(d * cw, TB, (d + 1) * cw - 8, TB + WEEKHEAD), gray);
    long long today = DayStart(NowMin());
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    for (int i = 0; i < 42; i++) {
        long long day = AddDays(first, i);
        SYSTEMTIME s = FromMin(day);
        D2D1_RECT_F c = D2D1::RectF((i % 7) * cw, top + (i / 7) * ch, (i % 7 + 1) * cw, top + (i / 7 + 1) * ch);
        bool other = s.wMonth != am.wMonth;
        if (s.wDayOfWeek == 0 || s.wDayOfWeek == 6) Fill(c, dark ? Rgb(255, 255, 255, 0.025f) : Rgb(0, 0, 0, 0.025f));
        if (day == g_selDay) Fill(c, dark ? Rgb(10, 132, 255, 0.16f) : Rgb(10, 132, 255, 0.08f));
        Fill(D2D1::RectF(c.left, c.top, c.right, c.top + 1), line);
        Fill(D2D1::RectF(c.right - 1, c.top, c.right, c.bottom), line);
        // número do dia (hoje: no círculo vermelho)
        std::wstring n = std::to_wstring(s.wDay);
        if (s.wDay == 1) { std::wstring mn = MonthName(s.wMonth); n = n + L" " + mn.substr(0, 3) + L"."; }
        D2D1_RECT_F nr = D2D1::RectF(c.right - 70, c.top + 4, c.right - 8, c.top + 26);
        if (day == today) {
            float w = TextW(g_tfDayNum, n) + 12;
            Fill(D2D1::RectF(c.right - 8 - w, c.top + 4, c.right - 8, c.top + 26), Rgb(255, 59, 48), 11);
            Text(n, g_tfDayNum, D2D1::RectF(c.right - 8 - w, c.top + 4, c.right - 14, c.top + 26), Rgb(255, 255, 255));
        } else Text(n, g_tfDayNum, nr, other ? gray : text);
        g_hits.push_back({ 2, -1, day, c });
        // eventos do dia
        float y = c.top + 30, lineH = 18;
        int shown = 0, total = 0;
        std::vector<const Ev*> list;
        for (auto& e : g_ev) if (OnDay(e, day) && CalOn(e)) list.push_back(&e);
        std::sort(list.begin(), list.end(), [](const Ev* a, const Ev* b) { return a->allDay != b->allDay ? a->allDay : a->start < b->start; });
        std::vector<const RemItem*> rl;
        for (auto& r : rems) if (DayStart(r.due) == day) rl.push_back(&r);
        total = (int)(list.size() + rl.size());
        int capacity = std::max(0, (int)((c.bottom - y - 4) / lineH));
        for (auto* e : list) {
            if (shown >= capacity - (total > capacity ? 1 : 0)) break;
            D2D1_RECT_F er = D2D1::RectF(c.left + 4, y, c.right - 4, y + lineH - 2);
            D2D1_COLOR_F col = EvColor(e->color);
            bool sel = e->id == g_selEv;
            if (e->allDay || sel) Fill(er, sel ? col : D2D1::ColorF(col.r, col.g, col.b, dark ? 0.35f : 0.22f), 4);
            else Circle({ er.left + 6, (er.top + er.bottom) / 2 }, 3.5f, col, true);
            std::wstring t = e->allDay ? e->title : Hm(e->start) + L"  " + e->title;
            if (e->id != g_editing) Text(t, g_tfEv, D2D1::RectF(er.left + (e->allDay || sel ? 6 : 14), er.top, er.right - 2, er.bottom), sel ? Rgb(255, 255, 255) : text);
            g_hits.push_back({ 1, e->id, day, er });
            y += lineH; shown++;
        }
        for (auto* r : rl) {
            if (shown >= capacity - (total > capacity ? 1 : 0)) break;
            D2D1_RECT_F er = D2D1::RectF(c.left + 4, y, c.right - 4, y + lineH - 2);
            Text(std::wstring(r->done ? L"☑ " : L"☐ ") + Hm(r->due) + L"  " + r->title, g_tfEv, D2D1::RectF(er.left + 4, er.top, er.right, er.bottom), gray);
            y += lineH; shown++;
        }
        if (total > shown) { wchar_t b[32]; swprintf_s(b, Tr(L"mais %d…"), total - shown); Text(b, g_tfEvSmall, D2D1::RectF(c.left + 8, y, c.right - 4, y + lineH - 2), gray); }
    }
}

void PaintGrid(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray, const std::vector<RemItem>& rems) {
    int cols = g_view == V_DAY ? 1 : 7;
    long long first = g_view == V_DAY ? DayStart(g_anchor) : WeekStart(g_anchor), today = DayStart(NowMin());
    float cw = (sz.width - LEFTCOL) / cols;
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    // cabeçalho com os dias e a faixa de "dia inteiro"
    for (int d = 0; d < cols; d++) {
        long long day = AddDays(first, d);
        SYSTEMTIME s = FromMin(day);
        D2D1_RECT_F h = D2D1::RectF(LEFTCOL + d * cw, TB, LEFTCOL + (d + 1) * cw, TB + WEEKHEAD);
        wchar_t b[48]; swprintf_s(b, L"%ls %d", WeekdayShort(s.wDayOfWeek), s.wDay);
        if (day == today) Text(b, g_tfHeadDay, h, Rgb(255, 59, 48)); else Text(b, g_tfHeadDay, h, text);
        g_hits.push_back({ 2, -1, day, D2D1::RectF(h.left, TB + WEEKHEAD, h.right, TB + WEEKHEAD + ALLDAY) });
        float x = h.left + 2;
        for (auto& e : g_ev) if (e.allDay && OnDay(e, day) && CalOn(e)) {
            D2D1_RECT_F er = D2D1::RectF(x, TB + WEEKHEAD + 3, h.right - 2, TB + WEEKHEAD + ALLDAY - 3);
            D2D1_COLOR_F col = EvColor(e.color);
            Fill(er, e.id == g_selEv ? col : D2D1::ColorF(col.r, col.g, col.b, 0.3f), 4);
            if (e.id != g_editing) Text(e.title, g_tfEv, D2D1::RectF(er.left + 6, er.top, er.right, er.bottom), e.id == g_selEv ? Rgb(255, 255, 255) : text);
            g_hits.push_back({ 1, e.id, day, er });
            break;  // uma por coluna na faixa (cabe pouco)
        }
    }
    float top = TB + WEEKHEAD + ALLDAY;
    Fill(D2D1::RectF(0, top - 1, sz.width, top), line);
    rt->PushAxisAlignedClip(D2D1::RectF(0, top, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = top - g_scrollH;
    for (int hr = 0; hr < 24; hr++) {
        float y = y0 + hr * HOUR;
        Fill(D2D1::RectF(LEFTCOL, y, sz.width, y + 1), line);
        if (hr) { wchar_t b[8]; swprintf_s(b, L"%d:00", hr); Text(b, g_tfHour, D2D1::RectF(0, y - 9, LEFTCOL - 8, y + 9), gray); }
    }
    for (int d = 0; d < cols; d++) {
        long long day = AddDays(first, d);
        float x = LEFTCOL + d * cw;
        Fill(D2D1::RectF(x, top, x + 1, sz.height), line);
        g_hits.push_back({ 2, -1, day, D2D1::RectF(x, top, x + cw, sz.height) });
        for (auto& e : g_ev) {
            if (e.allDay || !OnDay(e, day) || !CalOn(e)) continue;
            long long s = std::max(e.start, day), en = std::min(std::max(e.end, e.start + 30), AddDays(day, 1));
            float ya = y0 + (s - day) / 60.f * HOUR, yb = y0 + (en - day) / 60.f * HOUR;
            D2D1_RECT_F er = D2D1::RectF(x + 2, ya + 1, x + cw - 3, std::max(ya + 18, yb - 1));
            D2D1_COLOR_F col = EvColor(e.color);
            bool sel = e.id == g_selEv;
            Fill(er, sel ? col : D2D1::ColorF(col.r, col.g, col.b, dark ? 0.35f : 0.2f), 5);
            Fill(D2D1::RectF(er.left, er.top, er.left + 3, er.bottom), col, 1.5f);
            if (e.id != g_editing) {
                Text(e.title, g_tfEv, D2D1::RectF(er.left + 8, er.top + 2, er.right - 2, er.top + 18), sel ? Rgb(255, 255, 255) : text);
                if (er.bottom - er.top > 34) Text(Hm(e.start) + L" – " + Hm(e.end), g_tfEvSmall, D2D1::RectF(er.left + 8, er.top + 18, er.right - 2, er.top + 34), sel ? Rgb(255, 255, 255) : gray);
            }
            g_hits.push_back({ 1, e.id, day, er });
        }
        for (auto& r : rems) if (DayStart(r.due) == day) {
            float ya = y0 + (r.due - day) / 60.f * HOUR;
            Text(std::wstring(r.done ? L"☑ " : L"☐ ") + r.title, g_tfEv, D2D1::RectF(x + 6, ya, x + cw - 4, ya + 18), gray);
        }
        if (day == today) {  // linha vermelha da hora atual
            long long now = NowMin();
            float y = y0 + (now - day) / 60.f * HOUR;
            Fill(D2D1::RectF(x, y - 1, x + cw, y + 1), Rgb(255, 59, 48));
            Circle({ x + 2, y }, 4, Rgb(255, 59, 48), true);
        }
    }
    rt->PopAxisAlignedClip();
}

// Ano: os 12 meses em miniatura (4 × 3); hoje no círculo vermelho; um ponto nos dias com eventos. Clique: o Dia.
void PaintYear(D2D1_SIZE_F sz, bool dark, D2D1_COLOR_F text, D2D1_COLOR_F gray) {
    SYSTEMTIME a = FromMin(g_anchor);
    float top = TB + 8, mw = (sz.width - 40) / 4, mh = (sz.height - top - 8) / 3;
    long long today = DayStart(NowMin());
    SYSTEMTIME tn = FromMin(today);
    for (int m = 0; m < 12; m++) {
        float x0 = 20 + (m % 4) * mw, y0 = top + (m / 4) * mh;
        std::wstring mn = MonthName(m + 1);
        if (!idioma::En() && !mn.empty()) mn[0] = towupper(mn[0]);
        bool cur = a.wYear == tn.wYear && m + 1 == tn.wMonth;
        Text(mn, g_tfHeadDay, D2D1::RectF(x0 + 6, y0 + 4, x0 + mw - 10, y0 + 26), cur ? Rgb(255, 59, 48) : text);
        if (g_tfHeadDay) {}
        SYSTEMTIME s1{}; s1.wYear = a.wYear; s1.wMonth = (WORD)(m + 1); s1.wDay = 1;
        long long first = ToMin(s1), next = AddMonths(first, 1);
        int wd = FromMin(first).wDayOfWeek, days = (int)((DayStart(next + 12 * 60) - first + 12 * 60) / (24 * 60));
        float cw = (mw - 20) / 7, ch = std::min((mh - 50) / 6, cw);
        for (int d = 0; d < 7; d++) Text(std::wstring(1, WeekdayShort(d)[0]), g_tfYearDay, D2D1::RectF(x0 + d * cw, y0 + 26, x0 + (d + 1) * cw, y0 + 42), gray);
        for (int d = 1; d <= days; d++) {
            int idx = wd + d - 1;
            D2D1_RECT_F c = D2D1::RectF(x0 + (idx % 7) * cw, y0 + 44 + (idx / 7) * ch, x0 + (idx % 7 + 1) * cw, y0 + 44 + (idx / 7 + 1) * ch);
            long long day = AddDays(first, d - 1);
            bool has = false; for (auto& e : g_ev) if (CalOn(e) && OnDay(e, day)) { has = true; break; }
            if (day == today) Circle({ (c.left + c.right) / 2, (c.top + c.bottom) / 2 }, std::min(cw, ch) * 0.42f, Rgb(255, 59, 48), true);
            Text(std::to_wstring(d), g_tfYearDay, c, day == today ? Rgb(255, 255, 255) : text);
            if (has && day != today) Circle({ (c.left + c.right) / 2, c.bottom - 2 }, 1.8f, Rgb(10, 132, 255), true);
            g_hits.push_back({ 8, -1, day, c });
        }
    }
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    auto rems = Reminders();
    // conteúdo deslocado pela barra lateral (os retângulos clicáveis também)
    float X0 = g_side ? SIDE : 0;
    D2D1_SIZE_F csz{ sz.width - X0, sz.height };
    rt->SetTransform(D2D1::Matrix3x2F::Translation(X0, 0));
    size_t h0 = g_hits.size();
    if (g_view == V_YEAR) PaintYear(csz, dark, text, gray);
    else if (g_view == V_MONTH) PaintMonth(csz, dark, text, gray, rems); else PaintGrid(csz, dark, text, gray, rems);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    for (size_t i = h0; i < g_hits.size(); i++) { g_hits[i].r.left += X0; g_hits[i].r.right += X0; }
    if (g_side) {  // calendários
        Fill(D2D1::RectF(0, TB, SIDE, sz.height), SidebarColor());
        Fill(D2D1::RectF(SIDE - 1, TB, SIDE, sz.height), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.08f));
        Text(Tr(L"Calendários"), g_tfEv, D2D1::RectF(14, TB + 10, SIDE - 14, TB + 30), gray);
        if (g_tfHeadDay) {}
        float y = TB + 36;
        for (int i = 0; i < (int)g_cals.size(); i++) {
            auto& c = g_cals[i];
            D2D1_RECT_F r = D2D1::RectF(8, y, SIDE - 8, y + 28);
            D2D1_RECT_F box = D2D1::RectF(r.left + 8, y + 7, r.left + 22, y + 21);
            D2D1_COLOR_F col = EvColor(c.color);
            if (c.on) { Fill(box, col, 3.5f); Text(L"✓", g_tfEvSmall, box, Rgb(255, 255, 255)); }
            else { br->SetColor(col); rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), br, 1.5f); }
            Text(c.name, g_tfEv, D2D1::RectF(r.left + 32, r.top, r.right, r.bottom), text);
            g_hits.push_back({ 6, i, 0, r });
            y += 30;
        }
        D2D1_RECT_F add = D2D1::RectF(14, y + 6, SIDE - 14, y + 30);
        Text(L"⊕  " + std::wstring(Tr(L"Novo Calendário")), g_tfEv, add, gray);
        g_hits.push_back({ 7, 0, 0, add });
    }
    Fill(D2D1::RectF(0, 0, sz.width, TB), dark ? Rgb(40, 40, 42) : Rgb(246, 246, 248));
    Fill(D2D1::RectF(0, TB - 1, sz.width, TB), dark ? Rgb(0, 0, 0, 0.5f) : Rgb(0, 0, 0, 0.1f));
    D2D1_RECT_F tog = D2D1::RectF(10, 18, 40, 44);  // mostrar/esconder a barra lateral
    wchar_t sg[2] = { 0xE700, 0 }; Text(sg, g_tfIcon, tog, g_side ? Rgb(10, 132, 255) : text);
    g_hits.push_back({ 5, 0, 0, tog });
    rt->SetTransform(D2D1::Matrix3x2F::Translation(36, 0));
    size_t t0 = g_hits.size();
    Toolbar(D2D1::SizeF(sz.width - 36, sz.height), dark, text, gray);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    for (size_t i = t0; i < g_hits.size(); i++) { g_hits[i].r.left += 36; g_hits[i].r.right += 36; }
    EndPaint();
}

// ---- ações
void Rename(int id) {
    for (auto& h : g_hits) if (h.kind == 1 && h.ev == id) {
        Ev* e = FindEv(id);
        g_editing = id;
        FieldOpen(D2D1::RectF(h.r.left + 4, h.r.top, std::max(h.r.right, h.r.left + 160), h.r.top + 18), e ? e->title : L"", 12, false, [id](const std::wstring& t, int key) {
            g_editing = -1;
            Ev* e = FindEv(id);
            if (!e) return;
            if (t.empty() && key != VK_ESCAPE) { g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end()); g_selEv = -1; }
            else if (!t.empty()) e->title = t;
            Save();
        });
        InvalidateRect(wnd, nullptr, FALSE);
        return;
    }
}

void NewEvent(long long day, long long at, bool allDay) {
    Ev e{ g_nextId++, allDay ? day : at, allDay ? AddDays(day, 1) : at + 60, allDay, Tr(L"Novo Evento"), 0, L"" };
    for (auto& c : g_cals) if (c.on) { e.cal = c.id; e.color = c.color; break; }  // no primeiro calendário à vista
    g_ev.push_back(e);
    g_selEv = e.id; g_selDay = day;
    Save();
    Paint();
    Rename(e.id);
}

void EvMenu(int id, POINT scr) {
    Ev* e = FindEv(id);
    if (!e) return;
    HMENU m = CreatePopupMenu(), tm = CreatePopupMenu(), dur = CreatePopupMenu(), col = CreatePopupMenu();
    AppendMenuW(tm, MF_STRING | (e->allDay ? MF_CHECKED : 0), 100, Tr(L"Dia Inteiro"));
    AppendMenuW(tm, MF_SEPARATOR, 0, nullptr);
    for (int h = 7; h <= 21; h++) { wchar_t b[16]; swprintf_s(b, L"%d:00", h); AppendMenuW(tm, MF_STRING, 100 + h, b); }
    int mins[] = { 15, 30, 60, 90, 120, 180 };
    const wchar_t* dn[] = { L"15 min", L"30 min", L"1 h", L"1 h 30", L"2 h", L"3 h" };
    for (int i = 0; i < 6; i++) AppendMenuW(dur, MF_STRING | (e->allDay ? MF_GRAYED : 0), 200 + i, dn[i]);
    for (int i = 0; i < 7; i++) AppendMenuW(col, MF_STRING | (e->color == i ? MF_CHECKED : 0), 300 + i, Tr(COLOR_NAMES[i]));
    HMENU cal = CreatePopupMenu();
    for (int i = 0; i < (int)g_cals.size(); i++) AppendMenuW(cal, MF_STRING | (e->cal == g_cals[i].id ? MF_CHECKED : 0), 500 + i, g_cals[i].name.c_str());
    AppendMenuW(m, MF_STRING, 1, Tr(L"Renomear"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, Tr(L"Horário"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dur, Tr(L"Duração"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cal, Tr(L"Calendário"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)col, Tr(L"Cor"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, Tr(L"Apagar"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    long long day = DayStart(e->start);
    if (c == 100) { e->allDay = true; e->start = day; e->end = AddDays(day, 1); }
    else if (c > 100 && c < 200) { long long len = e->allDay ? 60 : std::max(15LL, e->end - e->start); e->allDay = false; e->start = day + (c - 100) * 60; e->end = e->start + len; }
    else if (c >= 200 && c < 206) e->end = e->start + mins[c - 200];
    else if (c >= 300 && c < 307) e->color = c - 300;
    else if (c >= 500 && c < 500 + (int)g_cals.size()) { e->cal = g_cals[c - 500].id; e->color = g_cals[c - 500].color; }
    else if (c == 2) { g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end()); g_selEv = -1; }
    else if (c == 1) { Paint(); Rename(id); return; }
    if (c) Save();
    InvalidateRect(wnd, nullptr, FALSE);
}

void Nav(int dir) {
    if (dir == 0) { g_anchor = g_selDay = DayStart(NowMin()); }
    else if (g_view == V_YEAR) g_anchor = AddMonths(g_anchor, dir * 12);
    else if (g_view == V_MONTH) g_anchor = AddMonths(g_anchor, dir);
    else g_anchor = AddDays(DayStart(g_anchor), dir * (g_view == V_WEEK ? 7 : 1));
    InvalidateRect(wnd, nullptr, FALSE);
}

// ---- .ics (o básico: DTSTART, DTEND, SUMMARY, DESCRIPTION; datas locais ou UTC "Z")
long long IcsTime(const std::wstring& v, bool& allDay) {
    allDay = v.size() == 8;
    if (v.size() < 8) return 0;
    SYSTEMTIME s{}; s.wYear = (WORD)_wtoi(v.substr(0, 4).c_str()); s.wMonth = (WORD)_wtoi(v.substr(4, 2).c_str()); s.wDay = (WORD)_wtoi(v.substr(6, 2).c_str());
    if (v.size() >= 15) { s.wHour = (WORD)_wtoi(v.substr(9, 2).c_str()); s.wMinute = (WORD)_wtoi(v.substr(11, 2).c_str()); }
    if (!v.empty() && v.back() == L'Z') { SYSTEMTIME loc; SystemTimeToTzSpecificLocalTime(nullptr, &s, &loc); s = loc; }
    return ToMin(s);
}
void ImportIcs() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"iCalendar (.ics)", L"*.ics" } };
    d->SetFileTypes(1, f);
    std::wstring path;
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); } it->Release(); } }
    d->Release();
    if (path.empty()) return;
    auto lines = ReadLines(path);
    // linhas dobradas (começam com espaço) continuam a anterior
    std::vector<std::wstring> un;
    for (auto& l : lines) { if (!l.empty() && (l[0] == L' ' || l[0] == L'\t') && !un.empty()) un.back() += l.substr(1); else un.push_back(l); }
    int n = 0; Ev cur{}; bool in = false;
    for (auto& l : un) {
        size_t c = l.find(L':'); std::wstring key = c == std::wstring::npos ? l : l.substr(0, c), val = c == std::wstring::npos ? L"" : l.substr(c + 1);
        std::wstring k = key.substr(0, key.find(L';'));
        if (l == L"BEGIN:VEVENT") { cur = Ev{ 0, 0, 0, false, L"", 0, L"" }; in = true; }
        else if (l == L"END:VEVENT" && in) {
            in = false;
            if (cur.start) { cur.id = g_nextId++; if (!cur.end || cur.end <= cur.start) cur.end = cur.allDay ? AddDays(cur.start, 1) : cur.start + 60; g_ev.push_back(cur); n++; }
        } else if (in && k == L"DTSTART") { bool ad; cur.start = IcsTime(val, ad); cur.allDay = ad || key.find(L"VALUE=DATE") != std::wstring::npos; }
        else if (in && k == L"DTEND") { bool ad; cur.end = IcsTime(val, ad); }
        else if (in && k == L"SUMMARY") { std::wstring t; for (size_t i = 0; i < val.size(); i++) { if (val[i] == L'\\' && i + 1 < val.size()) { i++; t += val[i] == L'n' ? L' ' : val[i]; } else t += val[i]; } cur.title = t; }
    }
    Save();
    wchar_t b[96]; swprintf_s(b, Tr(L"%d eventos importados."), n);
    MessageBoxW(wnd, b, Tr(L"Calendário"), MB_OK | MB_ICONINFORMATION);
    InvalidateRect(wnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CTLCOLOREDIT: return FieldColors((HDC)w, Dark() ? RGB(30, 30, 30) : RGB(255, 255, 255));
    case WM_FIELDLOST: if (field.h && GetFocus() != field.h) FieldEnd(0); return 0;
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;  // a linha da hora atual e os lembretes
    case WM_MOUSEWHEEL: {
        if (field.h) FieldEnd(0);
        float d = GET_WHEEL_DELTA_WPARAM(w) / 120.f;
        if (g_view == V_MONTH) Nav(d > 0 ? -1 : 1);
        else { g_scrollH = std::clamp(g_scrollH - d * 60, 0.f, std::max(0.f, 24 * HOUR - (Size().height - TB - WEEKHEAD - ALLDAY))); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        bool dbl = m == WM_LBUTTONDBLCLK, right = m == WM_RBUTTONUP;
        for (auto& ht : g_hits) if (ht.kind == 4 && In(ht.r, x, y)) { g_view = (View)ht.ev; InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 5 && In(ht.r, x, y)) { g_side = !g_side; InvalidateRect(h, nullptr, FALSE); return 0; }
        for (auto& ht : g_hits) if (ht.kind == 8 && In(ht.r, x, y)) { g_anchor = g_selDay = ht.day; g_view = V_DAY; InvalidateRect(h, nullptr, FALSE); return 0; }  // Ano: o dia
        for (auto& ht : g_hits) if (ht.kind == 7 && In(ht.r, x, y)) {
            Cal c{ g_nextId++, Tr(L"Novo Calendário"), (int)g_cals.size() % 7, true };
            g_cals.push_back(c); Save(); Paint();
            for (auto& k : g_hits) if (k.kind == 6 && k.ev == (int)g_cals.size() - 1) {
                int id = c.id;
                FieldOpen(D2D1::RectF(k.r.left + 32, k.r.top, k.r.right, k.r.bottom), c.name, 12, false, [id](const std::wstring& t, int key) { if (key != VK_ESCAPE && !t.empty()) if (Cal* x = FindCal(id)) { x->name = t; Save(); } });
            }
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 6 && In(ht.r, x, y)) {
            Cal& c = g_cals[ht.ev];
            if (!right) { c.on = !c.on; Save(); InvalidateRect(h, nullptr, FALSE); return 0; }
            HMENU mm = CreatePopupMenu(), col = CreatePopupMenu();
            for (int i = 0; i < 7; i++) AppendMenuW(col, MF_STRING | (c.color == i ? MF_CHECKED : 0), 10 + i, Tr(COLOR_NAMES[i]));
            AppendMenuW(mm, MF_STRING, 1, Tr(L"Renomear"));
            AppendMenuW(mm, MF_POPUP, (UINT_PTR)col, Tr(L"Cor"));
            AppendMenuW(mm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(mm, MF_STRING | (g_cals.size() > 1 ? 0 : MF_GRAYED), 2, Tr(L"Apagar Calendário"));
            int k = TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr);
            DestroyMenu(mm);
            int id = c.id;
            if (k >= 10) { c.color = k - 10; for (auto& e : g_ev) if (e.cal == id) e.color = c.color; Save(); }
            else if (k == 1) FieldOpen(D2D1::RectF(ht.r.left + 32, ht.r.top, ht.r.right, ht.r.bottom), c.name, 12, false, [id](const std::wstring& t, int key) { if (key != VK_ESCAPE && !t.empty()) if (Cal* x = FindCal(id)) { x->name = t; Save(); } });
            else if (k == 2) {
                int n = 0; for (auto& e : g_ev) if (e.cal == id) n++;
                wchar_t q[200]; swprintf_s(q, Tr(L"Apagar o calendário “%ls” e os %d eventos dele?"), c.name.c_str(), n);
                if (MessageBoxW(h, q, Tr(L"Calendário"), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                    g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& e) { return e.cal == id; }), g_ev.end());
                    g_cals.erase(g_cals.begin() + ht.ev); Save();
                }
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 3 && In(ht.r, x, y)) { Nav(ht.ev); return 0; }
        for (int i = (int)g_hits.size() - 1; i >= 0; i--) {
            auto& ht = g_hits[i];
            if (ht.kind != 1 || !In(ht.r, x, y)) continue;
            g_selEv = ht.ev; g_selDay = ht.day;
            if (right) { InvalidateRect(h, nullptr, FALSE); EvMenu(ht.ev, scr); return 0; }
            if (dbl) { Rename(ht.ev); return 0; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        for (auto& ht : g_hits) if (ht.kind == 2 && In(ht.r, x, y)) {
            g_selDay = ht.day; g_selEv = -1;
            if (dbl) {
                if (g_view == V_MONTH || y < TB + WEEKHEAD + ALLDAY) NewEvent(ht.day, 0, true);
                else {
                    float top = TB + WEEKHEAD + ALLDAY;
                    int mins = (int)((y - top + g_scrollH) / HOUR * 60) / 30 * 30;
                    NewEvent(ht.day, ht.day + mins, false);
                }
                return 0;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w >= '1' && w <= '4') { g_view = (View)(w - '1'); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (ctrl && w == 'I') { ImportIcs(); return 0; }
        if (ctrl && w == 'N') { NewEvent(g_selDay, g_selDay + 9 * 60, g_view == V_MONTH); return 0; }
        if (ctrl && w == 'W') { DestroyWindow(h); return 0; }
        if (ctrl && w == 'T') { Nav(0); return 0; }
        if (w == VK_LEFT) Nav(-1);
        else if (w == VK_RIGHT) Nav(1);
        else if ((w == VK_DELETE || w == VK_BACK) && g_selEv >= 0) {
            int id = g_selEv;
            g_ev.erase(std::remove_if(g_ev.begin(), g_ev.end(), [id](const Ev& x) { return x.id == id; }), g_ev.end());
            g_selEv = -1; Save(); InvalidateRect(h, nullptr, FALSE);
        } else if (w == VK_RETURN && g_selEv >= 0) Rename(g_selEv);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"calendario.txt");
    g_remFile = DataPath(L"lembretes.txt");
    Load();
    g_anchor = g_selDay = DayStart(NowMin());
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfMonth = Fmt(26, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfYear = Fmt(26, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfBtn = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfHeadDay = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfDayNum = Fmt(13, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfEv = Fmt(11.5f);
    g_tfEvSmall = Fmt(10.5f);
    g_tfYearDay = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfHour = Fmt(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfIcon = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    if (g_tfMonth) { g_tfMonth->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); }
    if (!CreateMain(hi, L"CalendarioJanela", Tr(L"Calendário"), Proc, 1100, 760)) return 1;
    SetTimer(wnd, 1, 30000, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
