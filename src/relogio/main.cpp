// Relógio (Clock do Mac): Relógio Mundial, Alarmes, Cronômetro e Timer, nas abas do alto (Ctrl+1..4).
//  - Mundial: cidades (fusos do Windows), hora, "Hoje/Amanhã" e a diferença; + adiciona, botão direito remove.
//  - Alarmes: hora grande, nome, dias e o interruptor; + cria, clique edita a hora, botão direito apaga/renomeia.
//    Quem toca é o Shell (banner do Mac + som), mesmo com o app fechado: %APPDATA%\Shell\alarmes.txt.
//  - Cronômetro: Iniciar/Parar, Volta/Zerar e a lista de voltas (a melhor em verde, a pior em vermelho).
//  - Timer: escolher minutos (atalhos e − / +), Iniciar/Pausar/Cancelar; ao acabar toca e a janela vem para frente.
#include "../comum/app2d.h"
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

using namespace app2d;

namespace {

const float TB = 56;
const UINT WM_FIELDLOST = WM_APP + 77;
int g_tab = 0;  // 0 mundial, 1 alarmes, 2 cronômetro, 3 timer
IDWriteTextFormat *g_tfSeg, *g_tfBig, *g_tfBigR, *g_tfHuge, *g_tfRow, *g_tfSub, *g_tfBtn, *g_tfIcon, *g_tfMono;

// ---- relógio mundial (fusos do Windows por cidade)
struct City { const wchar_t* name; const wchar_t* tz; };
const City CITIES[] = {
    { L"São Paulo", L"E. South America Standard Time" }, { L"Rio de Janeiro", L"E. South America Standard Time" }, { L"Manaus", L"SA Western Standard Time" },
    { L"Fortaleza", L"SA Eastern Standard Time" }, { L"Lisboa", L"GMT Standard Time" }, { L"Londres", L"GMT Standard Time" }, { L"Paris", L"Romance Standard Time" },
    { L"Berlim", L"W. Europe Standard Time" }, { L"Madri", L"Romance Standard Time" }, { L"Roma", L"W. Europe Standard Time" }, { L"Atenas", L"GTB Standard Time" },
    { L"Istambul", L"Turkey Standard Time" }, { L"Moscou", L"Russian Standard Time" }, { L"Dubai", L"Arabian Standard Time" }, { L"Mumbai", L"India Standard Time" },
    { L"Singapura", L"Singapore Standard Time" }, { L"Pequim", L"China Standard Time" }, { L"Tóquio", L"Tokyo Standard Time" }, { L"Seul", L"Korea Standard Time" },
    { L"Sydney", L"AUS Eastern Standard Time" }, { L"Auckland", L"New Zealand Standard Time" }, { L"Honolulu", L"Hawaiian Standard Time" },
    { L"Los Angeles", L"Pacific Standard Time" }, { L"Denver", L"Mountain Standard Time" }, { L"Chicago", L"Central Standard Time" },
    { L"Nova York", L"Eastern Standard Time" }, { L"Cidade do México", L"Central Standard Time (Mexico)" }, { L"Bogotá", L"SA Pacific Standard Time" },
    { L"Buenos Aires", L"Argentina Standard Time" }, { L"Santiago", L"Pacific SA Standard Time" }, { L"Joanesburgo", L"South Africa Standard Time" },
    { L"Cairo", L"Egypt Standard Time" }, { L"Nairóbi", L"E. Africa Standard Time" }, { L"UTC", L"UTC" },
};
const int NCITIES = (int)(sizeof(CITIES) / sizeof(CITIES[0]));
std::vector<int> g_world;  // índices em CITIES

bool CityTime(int i, SYSTEMTIME& out) {
    DYNAMIC_TIME_ZONE_INFORMATION d{};
    for (DWORD k = 0; EnumDynamicTimeZoneInformation(k, &d) == ERROR_SUCCESS; k++)
        if (!wcscmp(d.TimeZoneKeyName, CITIES[i].tz)) {
            SYSTEMTIME utc; GetSystemTime(&utc);
            return SystemTimeToTzSpecificLocalTimeEx(&d, &utc, &out) != FALSE;
        }
    return false;
}

// ---- alarmes
struct Alarm { int id; int hh, mm; bool on; std::wstring label; int days; };  // days: bits 0 = dom ... 6 = sáb; 0 = uma vez
std::vector<Alarm> g_alarms;
int g_nextId = 1;
std::wstring g_file;

void Save() {
    std::vector<std::wstring> out{ L"# Relógio do WacOS: W = cidade do Relógio Mundial, A = alarme (id, hora, minuto, ligado, nome, dias)" };
    for (int i : g_world) out.push_back(L"W\t" + std::to_wstring(i));
    for (auto& a : g_alarms) out.push_back(L"A\t" + std::to_wstring(a.id) + L"\t" + std::to_wstring(a.hh) + L"\t" + std::to_wstring(a.mm) + L"\t" + (a.on ? L"1" : L"0") + L"\t" + Esc(a.label) + L"\t" + std::to_wstring(a.days));
    WriteLines(g_file, out);
}
void Load() {
    g_world.clear(); g_alarms.clear();
    bool any = false;
    for (auto& l : ReadLines(g_file)) {
        auto f = Split(l);
        if (f.size() >= 2 && f[0] == L"W") { int i = _wtoi(f[1].c_str()); if (i >= 0 && i < NCITIES) g_world.push_back(i); any = true; }
        else if (f.size() >= 7 && f[0] == L"A") g_alarms.push_back({ _wtoi(f[1].c_str()), _wtoi(f[2].c_str()), _wtoi(f[3].c_str()), f[4] == L"1", Unesc(f[5]), _wtoi(f[6].c_str()) });
    }
    if (!any && g_world.empty()) g_world = { 0, 5, 25, 17 };  // São Paulo, Londres, Nova York, Tóquio
    for (auto& a : g_alarms) g_nextId = std::max(g_nextId, a.id + 1);
}

// ---- cronômetro
bool g_swRun = false;
ULONGLONG g_swStart = 0, g_swAcc = 0;
std::vector<ULONGLONG> g_laps;  // duração de cada volta
ULONGLONG SwNow() { return g_swAcc + (g_swRun ? GetTickCount64() - g_swStart : 0); }
std::wstring FmtTime(ULONGLONG ms, bool cs = true) {
    wchar_t b[32];
    ULONGLONG s = ms / 1000;
    if (s >= 3600) swprintf_s(b, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else if (cs) swprintf_s(b, L"%02llu:%02llu,%02llu", s / 60, s % 60, ms / 10 % 100);
    else swprintf_s(b, L"%02llu:%02llu", s / 60, s % 60);
    return b;
}

// ---- timer
int g_tmMin = 5;               // escolhido
bool g_tmRun = false, g_tmPaused = false;
ULONGLONG g_tmEnd = 0, g_tmLeft = 0;  // fim (tick) / restante quando pausado
ULONGLONG TmLeft() { if (!g_tmRun) return (ULONGLONG)g_tmMin * 60000; if (g_tmPaused) return g_tmLeft; ULONGLONG n = GetTickCount64(); return g_tmEnd > n ? g_tmEnd - n : 0; }

struct Hit { int kind, arg; D2D1_RECT_F r; };
std::vector<Hit> g_hits;
enum { H_TAB = 1, H_ADD, H_WORLD, H_ALARM, H_ALARMTIME, H_SWITCH, H_SW_A, H_SW_B, H_TM_PRESET, H_TM_MINUS, H_TM_PLUS, H_TM_A, H_TM_B };

void Button(D2D1_RECT_F r, const std::wstring& t, D2D1_COLOR_F bg, D2D1_COLOR_F fg, int kind, int arg = 0) {
    float rad = (r.bottom - r.top) / 2;
    Fill(r, bg, rad);
    Text(t, g_tfBtn, r, fg);
    g_hits.push_back({ kind, arg, r });
}

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139);
    D2D1_COLOR_F line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    // abas
    const wchar_t* tabs[] = { L"Relógio Mundial", L"Alarmes", L"Cronômetro", L"Timer" };
    float segW = 120, sx = sz.width / 2 - segW * 2;
    Fill(D2D1::RectF(sx, 14, sx + segW * 4, 40), dark ? Rgb(58, 58, 60) : Rgb(232, 232, 237), 7);
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = D2D1::RectF(sx + i * segW + 2, 16, sx + (i + 1) * segW - 2, 38);
        if (g_tab == i) Fill(r, dark ? Rgb(99, 99, 102) : Rgb(255, 255, 255), 6);
        Text(Tr(tabs[i]), g_tfSeg, r, text);
        g_hits.push_back({ H_TAB, i, r });
    }
    if (g_tab <= 1) {
        D2D1_RECT_F add = D2D1::RectF(sz.width - 46, 12, sz.width - 14, 42);
        wchar_t p[2] = { 0xE710, 0 };
        Text(p, g_tfIcon, add, text);
        g_hits.push_back({ H_ADD, 0, add });
    }
    Fill(D2D1::RectF(0, TB - 1, sz.width, TB), line);
    float X = 28, W = sz.width - 28;
    if (g_tab == 0) {
        SYSTEMTIME here; GetLocalTime(&here);
        float y = TB + 8;
        for (int k = 0; k < (int)g_world.size(); k++) {
            int i = g_world[k];
            SYSTEMTIME t{};
            if (!CityTime(i, t)) continue;
            FILETIME a, b; SystemTimeToFileTime(&here, &a); SystemTimeToFileTime(&t, &b);
            long long diffMin = ((long long)(((ULONGLONG)b.dwHighDateTime << 32 | b.dwLowDateTime) - ((ULONGLONG)a.dwHighDateTime << 32 | a.dwLowDateTime))) / 600000000LL;
            long long dh = (diffMin + (diffMin >= 0 ? 15 : -15)) / 30;  // em meias horas
            std::wstring day = t.wDay == here.wDay ? Tr(L"Hoje") : (diffMin > 0 ? Tr(L"Amanhã") : Tr(L"Ontem"));
            wchar_t off[32];
            if (dh == 0) wcscpy_s(off, L"");
            else if (dh % 2) swprintf_s(off, L", %+lld h 30", dh / 2);
            else swprintf_s(off, L", %+lld h", dh / 2);
            D2D1_RECT_F row = D2D1::RectF(X, y, W, y + 74);
            Text(day + off, g_tfSub, D2D1::RectF(X, y + 10, W - 200, y + 30), gray);
            Text(Tr(CITIES[i].name), g_tfRow, D2D1::RectF(X, y + 30, W - 200, y + 60), text);
            wchar_t hm[16]; swprintf_s(hm, L"%d:%02d", t.wHour, t.wMinute);
            Text(hm, g_tfBigR, D2D1::RectF(W - 220, y, W, y + 74), text);
            Fill(D2D1::RectF(X, y + 74, W, y + 75), line);
            g_hits.push_back({ H_WORLD, k, row });
            y += 75;
        }
    } else if (g_tab == 1) {
        float y = TB + 8;
        if (g_alarms.empty()) Text(Tr(L"Nenhum Alarme"), g_tfRow, D2D1::RectF(0, TB, sz.width, sz.height * 0.6f), gray);
        for (int k = 0; k < (int)g_alarms.size(); k++) {
            auto& a = g_alarms[k];
            wchar_t hm[16]; swprintf_s(hm, L"%d:%02d", a.hh, a.mm);
            D2D1_RECT_F tr = D2D1::RectF(X, y, X + 220, y + 60);
            Text(hm, g_tfBig, tr, a.on ? text : gray);
            std::wstring sub = a.label.empty() ? Tr(L"Alarme") : a.label;
            if (a.days == 0x7F) sub += L", " + std::wstring(Tr(L"todos os dias"));
            else if (a.days == 0x3E) sub += L", " + std::wstring(Tr(L"dias de semana"));
            else if (a.days == 0x41) sub += L", " + std::wstring(Tr(L"fins de semana"));
            Text(sub, g_tfSub, D2D1::RectF(X, y + 56, W - 100, y + 76), gray);
            g_hits.push_back({ H_ALARMTIME, k, tr });
            // interruptor
            D2D1_RECT_F sw = D2D1::RectF(W - 60, y + 26, W - 10, y + 54);
            Fill(sw, a.on ? Rgb(48, 209, 88) : (dark ? Rgb(72, 72, 74) : Rgb(229, 229, 234)), 14);
            Circle({ a.on ? sw.right - 14 : sw.left + 14, (sw.top + sw.bottom) / 2 }, 12, Rgb(255, 255, 255), true);
            g_hits.push_back({ H_SWITCH, k, sw });
            g_hits.push_back({ H_ALARM, k, D2D1::RectF(X, y, W - 70, y + 84) });
            Fill(D2D1::RectF(X, y + 84, W, y + 85), line);
            y += 86;
        }
    } else if (g_tab == 2) {
        ULONGLONG now = SwNow();
        Text(FmtTime(now), g_tfHuge, D2D1::RectF(0, TB + 20, sz.width, TB + 150), text);
        float by = TB + 170, cx = sz.width / 2;
        bool running = g_swRun;
        Button(D2D1::RectF(cx - 190, by, cx - 30, by + 44), running ? Tr(L"Volta") : Tr(L"Zerar"), dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_SW_A);
        Button(D2D1::RectF(cx + 30, by, cx + 190, by + 44), running ? Tr(L"Parar") : Tr(L"Iniciar"), running ? Rgb(255, 69, 58, 0.25f) : Rgb(48, 209, 88, 0.25f), running ? Rgb(255, 69, 58) : Rgb(48, 209, 88), H_SW_B);
        // voltas (a atual em cima)
        std::vector<ULONGLONG> laps = g_laps;
        ULONGLONG done = 0; for (auto l : g_laps) done += l;
        if (now || !laps.empty()) laps.push_back(now - done);
        int best = -1, worst = -1;
        if (g_laps.size() >= 2) for (int i = 0; i < (int)g_laps.size(); i++) { if (best < 0 || g_laps[i] < g_laps[best]) best = i; if (worst < 0 || g_laps[i] > g_laps[worst]) worst = i; }
        float y = by + 70;
        for (int i = (int)laps.size() - 1; i >= 0 && y < sz.height - 20; i--) {
            D2D1_COLOR_F c = i == best ? Rgb(48, 209, 88) : i == worst ? Rgb(255, 69, 58) : text;
            wchar_t n[32]; swprintf_s(n, Tr(L"Volta %d"), i + 1);
            Text(n, g_tfRow, D2D1::RectF(X + 60, y, W, y + 34), c);
            Text(FmtTime(laps[i]), g_tfMono, D2D1::RectF(X, y, W - 60, y + 34), c);
            Fill(D2D1::RectF(X + 60, y + 34, W - 60, y + 35), line);
            y += 35;
        }
    } else {
        ULONGLONG left = TmLeft();
        Text(FmtTime(left + 999, false), g_tfHuge, D2D1::RectF(0, TB + 20, sz.width, TB + 150), g_tmRun && !g_tmPaused ? Rgb(255, 159, 10) : text);
        float cx = sz.width / 2, y = TB + 160;
        if (!g_tmRun) {
            int presets[] = { 1, 3, 5, 10, 15, 30, 60 };
            float pw = 64, px = cx - pw * 3.5f - 9;
            for (int i = 0; i < 7; i++) {
                D2D1_RECT_F r = D2D1::RectF(px + i * (pw + 3), y, px + i * (pw + 3) + pw, y + 34);
                bool on = g_tmMin == presets[i];
                wchar_t b[16]; swprintf_s(b, presets[i] >= 60 ? L"%d h" : L"%d min", presets[i] >= 60 ? presets[i] / 60 : presets[i]);
                Fill(r, on ? Rgb(255, 159, 10, 0.3f) : (dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234)), 8);
                Text(b, g_tfBtn, r, on ? Rgb(255, 159, 10) : text);
                g_hits.push_back({ H_TM_PRESET, presets[i], r });
            }
            Button(D2D1::RectF(cx - 90, y + 50, cx - 50, y + 84), L"−", dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_TM_MINUS);
            Button(D2D1::RectF(cx + 50, y + 50, cx + 90, y + 84), L"+", dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), text, H_TM_PLUS);
            wchar_t m[16]; swprintf_s(m, L"%d min", g_tmMin);
            Text(m, g_tfRow, D2D1::RectF(cx - 50, y + 50, cx + 50, y + 84), text);
            y += 110;
        }
        Button(D2D1::RectF(cx - 190, y, cx - 30, y + 44), Tr(L"Cancelar"), dark ? Rgb(58, 58, 60) : Rgb(229, 229, 234), g_tmRun ? text : gray, H_TM_A);
        const wchar_t* b = !g_tmRun ? Tr(L"Iniciar") : g_tmPaused ? Tr(L"Retomar") : Tr(L"Pausar");
        Button(D2D1::RectF(cx + 30, y, cx + 190, y + 44), b, g_tmRun && !g_tmPaused ? Rgb(255, 159, 10, 0.25f) : Rgb(48, 209, 88, 0.25f), g_tmRun && !g_tmPaused ? Rgb(255, 159, 10) : Rgb(48, 209, 88), H_TM_B);
    }
    EndPaint();
}

void Tick() {
    if (g_tmRun && !g_tmPaused && TmLeft() == 0) {  // timer acabou
        g_tmRun = false;
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
        FLASHWINFO fw{ sizeof(fw), wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 6, 0 }; FlashWindowEx(&fw);
        ShowWindow(wnd, SW_RESTORE); SetForegroundWindow(wnd);
        MessageBoxW(wnd, Tr(L"O timer terminou."), Tr(L"Relógio"), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
    static int n;
    if (g_swRun || (g_tmRun && !g_tmPaused) || ++n % 20 == 0) InvalidateRect(wnd, nullptr, FALSE);
}

void AddCity(POINT scr) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < NCITIES; i++) AppendMenuW(m, MF_STRING | (std::find(g_world.begin(), g_world.end(), i) != g_world.end() ? MF_CHECKED : 0), i + 1, Tr(CITIES[i].name));
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c > 0 && std::find(g_world.begin(), g_world.end(), c - 1) == g_world.end()) { g_world.push_back(c - 1); Save(); }
}

// hora do alarme digitada ("7:30", "0730", "19h")
bool ParseTime(const std::wstring& s, int& hh, int& mm) {
    std::wstring d; for (wchar_t c : s) if (iswdigit(c)) d += c;
    if (d.empty()) return false;
    if (d.size() <= 2) { hh = _wtoi(d.c_str()); mm = 0; }
    else { hh = _wtoi(d.substr(0, d.size() - 2).c_str()); mm = _wtoi(d.substr(d.size() - 2).c_str()); }
    return hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}
void EditAlarmTime(int k) {
    for (auto& h : g_hits) if (h.kind == H_ALARMTIME && h.arg == k) {
        int id = g_alarms[k].id;
        wchar_t cur[16]; swprintf_s(cur, L"%d:%02d", g_alarms[k].hh, g_alarms[k].mm);
        FieldOpen(D2D1::RectF(h.r.left, h.r.top + 10, h.r.left + 160, h.r.bottom - 10), cur, 30, false, [id](const std::wstring& t, int key) {
            if (key == VK_ESCAPE) return;
            for (auto& a : g_alarms) if (a.id == id) { int hh, mm; if (ParseTime(t, hh, mm)) { a.hh = hh; a.mm = mm; a.on = true; Save(); } }
        });
    }
}
void AddAlarm() {
    SYSTEMTIME t; GetLocalTime(&t);
    g_alarms.push_back({ g_nextId++, (t.wHour + 1) % 24, 0, true, L"", 0 });
    Save();
    Paint();
    EditAlarmTime((int)g_alarms.size() - 1);
}
void AlarmMenu(int k, POINT scr) {
    HMENU m = CreatePopupMenu(), rep = CreatePopupMenu();
    int d = g_alarms[k].days;
    AppendMenuW(rep, MF_STRING | (d == 0 ? MF_CHECKED : 0), 10, Tr(L"Nunca"));
    AppendMenuW(rep, MF_STRING | (d == 0x7F ? MF_CHECKED : 0), 11, Tr(L"Todos os Dias"));
    AppendMenuW(rep, MF_STRING | (d == 0x3E ? MF_CHECKED : 0), 12, Tr(L"Dias de Semana"));
    AppendMenuW(rep, MF_STRING | (d == 0x41 ? MF_CHECKED : 0), 13, Tr(L"Fins de Semana"));
    AppendMenuW(m, MF_STRING, 1, Tr(L"Editar Hora"));
    AppendMenuW(m, MF_STRING, 2, Tr(L"Renomear"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rep, Tr(L"Repetir"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, Tr(L"Apagar"));
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c == 1) EditAlarmTime(k);
    else if (c == 2) {
        for (auto& h : g_hits) if (h.kind == H_ALARM && h.arg == k) {
            int id = g_alarms[k].id;
            FieldOpen(D2D1::RectF(h.r.left, h.r.top + 56, h.r.right, h.r.top + 76), g_alarms[k].label, 13, false, [id](const std::wstring& t, int key) {
                if (key == VK_ESCAPE) return; for (auto& a : g_alarms) if (a.id == id) { a.label = t; Save(); } });
        }
    } else if (c == 3) { g_alarms.erase(g_alarms.begin() + k); Save(); }
    else if (c >= 10) { static const int v[] = { 0, 0x7F, 0x3E, 0x41 }; g_alarms[k].days = v[c - 10]; Save(); }
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
    case WM_TIMER: Tick(); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        bool right = m == WM_RBUTTONUP;
        for (auto& ht : g_hits) {
            if (!In(ht.r, x, y)) continue;
            switch (ht.kind) {
            case H_TAB: g_tab = ht.arg; break;
            case H_ADD: if (g_tab == 0) AddCity(scr); else AddAlarm(); break;
            case H_WORLD:
                if (right) {
                    HMENU mm = CreatePopupMenu(); AppendMenuW(mm, MF_STRING, 1, Tr(L"Apagar"));
                    if (TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr) == 1) { g_world.erase(g_world.begin() + ht.arg); Save(); }
                    DestroyMenu(mm);
                }
                break;
            case H_SWITCH: g_alarms[ht.arg].on = !g_alarms[ht.arg].on; Save(); break;
            case H_ALARMTIME: if (right) AlarmMenu(ht.arg, scr); else EditAlarmTime(ht.arg); break;
            case H_ALARM: if (right) AlarmMenu(ht.arg, scr); break;
            case H_SW_A: if (g_swRun) { ULONGLONG done = 0; for (auto v : g_laps) done += v; g_laps.push_back(SwNow() - done); } else { g_swAcc = 0; g_laps.clear(); } break;
            case H_SW_B: if (g_swRun) { g_swAcc = SwNow(); g_swRun = false; } else { g_swStart = GetTickCount64(); g_swRun = true; } break;
            case H_TM_PRESET: g_tmMin = ht.arg; break;
            case H_TM_MINUS: g_tmMin = std::max(1, g_tmMin - 1); break;
            case H_TM_PLUS: g_tmMin = std::min(24 * 60, g_tmMin + 1); break;
            case H_TM_A: g_tmRun = false; g_tmPaused = false; break;
            case H_TM_B:
                if (!g_tmRun) { g_tmRun = true; g_tmPaused = false; g_tmEnd = GetTickCount64() + (ULONGLONG)g_tmMin * 60000; }
                else if (g_tmPaused) { g_tmPaused = false; g_tmEnd = GetTickCount64() + g_tmLeft; }
                else { g_tmLeft = TmLeft(); g_tmPaused = true; }
                break;
            default: continue;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w >= '1' && w <= '4') { g_tab = (int)(w - '1'); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (ctrl && w == 'W') { DestroyWindow(h); return 0; }
        if (w == VK_SPACE && g_tab == 2) { { if (g_swRun) { g_swAcc = SwNow(); g_swRun = false; } else { g_swStart = GetTickCount64(); g_swRun = true; } } InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"relogio.txt");
    Load();
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfSeg = Fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBig = Fmt(44, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_TRAILING, false, disp.c_str());
    g_tfHuge = Fmt(84, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    if (g_tfHuge) { DWRITE_FONT_FEATURE f{ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 }; (void)f; }
    g_tfRow = Fmt(16);
    g_tfSub = Fmt(13);
    g_tfBtn = Fmt(15, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfMono = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!CreateMain(hi, L"RelogioJanela", Tr(L"Relógio"), Proc, 720, 640)) return 1;
    // alarmes: o tempo do Mac é a hora grande alinhada à esquerda
    if (g_tfBig) g_tfBig->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfBigR = Fmt(44, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_TRAILING, false, disp.c_str());
    SetTimer(wnd, 1, 50, nullptr);
    ShowWindow(wnd, show);
    return Run();
}
