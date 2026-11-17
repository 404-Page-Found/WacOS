// Contatos (Contacts do Mac): à esquerda a lista em ordem alfabética, com as letras e a busca; à direita o cartão:
// a foto (iniciais num círculo), o nome, a empresa e os campos (celular, e-mail, endereço, aniversário, notas).
// Clique num campo edita; "+" cria; botão direito na lista apaga. Ctrl+I importa .vcf (vCard), Ctrl+E exporta todos.
// Dados em %APPDATA%\Shell\contatos.txt.
#include "../comum/app2d.h"
#include <shobjidl.h>
#include <map>

using namespace app2d;

namespace {

const float SB = 260, SEARCH = 46, ROW = 30;
const UINT WM_FIELDLOST = WM_APP + 77;
enum Fld { F_NAME, F_COMPANY, F_PHONE, F_EMAIL, F_ADDRESS, F_BIRTHDAY, F_NOTES, F_COUNT };
const wchar_t* FIELD_LABELS[F_COUNT] = { L"nome", L"empresa", L"celular", L"e-mail", L"endereço", L"aniversário", L"notas" };
struct Contact { int id; std::wstring f[F_COUNT]; };
std::vector<Contact> g_c;
int g_nextId = 1, g_sel = -1;
std::wstring g_query, g_file;
float g_scroll = 0;
IDWriteTextFormat *g_tfCenter, *g_tfName, *g_tfRow, *g_tfLetter, *g_tfLabel, *g_tfValue, *g_tfInit, *g_tfIcon, *g_tfSmall;

void Save() {
    std::vector<std::wstring> out{ L"# Contatos do WacOS: P id nome empresa celular e-mail endereço aniversário notas" };
    for (auto& c : g_c) { std::wstring l = L"P\t" + std::to_wstring(c.id); for (auto& x : c.f) l += L"\t" + Esc(x); out.push_back(l); }
    WriteLines(g_file, out);
}
void Load() {
    g_c.clear();
    for (auto& l : ReadLines(g_file)) {
        auto f = Split(l);
        if (f.size() >= 2 + F_COUNT && f[0] == L"P") { Contact c{ _wtoi(f[1].c_str()) }; for (int i = 0; i < F_COUNT; i++) c.f[i] = Unesc(f[2 + i]); g_c.push_back(c); }
    }
    for (auto& c : g_c) g_nextId = std::max(g_nextId, c.id + 1);
}
Contact* Find(int id) { for (auto& c : g_c) if (c.id == id) return &c; return nullptr; }
std::wstring Fold(std::wstring s) {
    for (auto& c : s) { c = towlower(c); static const wchar_t* a = L"áàâãäéèêëíìîïóòôõöúùûüç", * b = L"aaaaaeeeeiiiiooooouuuuc"; const wchar_t* p = wcschr(a, c); if (p) c = b[p - a]; }
    return s;
}
std::vector<Contact*> Shown() {
    std::vector<Contact*> v;
    std::wstring q = Fold(g_query);
    for (auto& c : g_c) {
        if (!q.empty()) { bool hit = false; for (auto& x : c.f) if (Fold(x).find(q) != std::wstring::npos) hit = true; if (!hit) continue; }
        v.push_back(&c);
    }
    std::sort(v.begin(), v.end(), [](Contact* a, Contact* b) { return Fold(a->f[F_NAME]) < Fold(b->f[F_NAME]); });
    return v;
}
std::wstring Initials(const std::wstring& n) {
    std::wstring r; bool start = true;
    for (wchar_t c : n) { if (c == L' ') start = true; else if (start) { r += towupper(c); start = false; if (r.size() == 2) break; } }
    return r.empty() ? L"?" : r;
}
D2D1_COLOR_F AvatarColor(const std::wstring& n) {
    static const D2D1_COLOR_F c[] = { Rgb(255, 149, 0), Rgb(255, 45, 85), Rgb(175, 82, 222), Rgb(0, 122, 255), Rgb(52, 199, 89), Rgb(90, 200, 250), Rgb(142, 142, 147) };
    unsigned h = 0; for (wchar_t x : n) h = h * 31 + x;
    return c[h % 7];
}

struct Hit { int kind, arg; D2D1_RECT_F r; };  // 1 contato, 2 campo, 3 +, 4 busca
std::vector<Hit> g_hits;

void Paint() {
    if (!BeginPaint()) return;
    g_hits.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = Size();
    D2D1_COLOR_F text = dark ? Rgb(245, 245, 247) : Rgb(29, 29, 31), gray = dark ? Rgb(152, 152, 157) : Rgb(134, 134, 139), line = dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.10f);
    rt->Clear(dark ? Rgb(30, 30, 30) : Rgb(255, 255, 255));
    Fill(D2D1::RectF(0, 0, SB, sz.height), SidebarColor());
    // busca e +
    D2D1_RECT_F sr = D2D1::RectF(10, 10, SB - 44, 36);
    Fill(sr, dark ? Rgb(58, 58, 60) : Rgb(228, 228, 233), 8);
    wchar_t mag[2] = { 0xE721, 0 }; Text(mag, g_tfSmall, D2D1::RectF(sr.left + 4, sr.top, sr.left + 26, sr.bottom), gray);
    Text(g_query.empty() ? std::wstring(Tr(L"Buscar")) : g_query, g_tfRow, D2D1::RectF(sr.left + 28, sr.top, sr.right - 6, sr.bottom), g_query.empty() ? gray : text);
    g_hits.push_back({ 4, 0, sr });
    D2D1_RECT_F add = D2D1::RectF(SB - 38, 8, SB - 8, 38);
    wchar_t p[2] = { 0xE710, 0 }; Text(p, g_tfIcon, add, text);
    g_hits.push_back({ 3, 0, add });
    // lista
    rt->PushAxisAlignedClip(D2D1::RectF(0, SEARCH, SB, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = SEARCH - g_scroll;
    wchar_t last = 0;
    for (Contact* c : Shown()) {
        std::wstring n = c->f[F_NAME].empty() ? Tr(L"Sem Nome") : c->f[F_NAME];
        wchar_t L0 = towupper(Fold(n)[0]);
        if (L0 != last) { last = L0; wchar_t b[2] = { L0, 0 }; Text(b, g_tfLetter, D2D1::RectF(16, y + 4, SB - 10, y + 24), gray); y += 26; }
        D2D1_RECT_F r = D2D1::RectF(8, y, SB - 8, y + ROW);
        if (c->id == g_sel) Fill(r, Rgb(10, 132, 255), 6);
        Text(n, g_tfRow, D2D1::RectF(r.left + 10, r.top, r.right - 6, r.bottom), c->id == g_sel ? Rgb(255, 255, 255) : text);
        g_hits.push_back({ 1, c->id, r });
        y += ROW;
    }
    rt->PopAxisAlignedClip();
    if (g_c.empty()) Text(Tr(L"Nenhum Contato"), g_tfCenter, D2D1::RectF(0, SEARCH, SB, sz.height * 0.6f), gray);
    // cartão
    Contact* c = Find(g_sel);
    if (!c) { Text(Tr(L"Nenhum Cartão Selecionado"), g_tfValue, D2D1::RectF(SB, 0, sz.width, sz.height), gray); EndPaint(); return; }
    float X = SB + 40, W = sz.width - 40;
    D2D1_POINT_2F av{ X + 44, 70 };
    Circle(av, 44, AvatarColor(c->f[F_NAME]), true);
    Text(Initials(c->f[F_NAME]), g_tfInit, D2D1::RectF(av.x - 44, av.y - 44, av.x + 44, av.y + 44), Rgb(255, 255, 255));
    D2D1_RECT_F nr = D2D1::RectF(X + 108, 40, W, 78);
    Text(c->f[F_NAME].empty() ? std::wstring(Tr(L"Sem Nome")) : c->f[F_NAME], g_tfName, nr, text);
    g_hits.push_back({ 2, F_NAME, nr });
    D2D1_RECT_F cr = D2D1::RectF(X + 108, 78, W, 100);
    Text(c->f[F_COMPANY].empty() ? std::wstring(Tr(L"empresa")) : c->f[F_COMPANY], g_tfValue, cr, c->f[F_COMPANY].empty() ? Rgb(150, 150, 150, 0.6f) : gray);
    g_hits.push_back({ 2, F_COMPANY, cr });
    float yy = 140;
    for (int i = F_PHONE; i < F_COUNT; i++) {
        Fill(D2D1::RectF(X, yy - 6, W, yy - 5), line);
        Text(Tr(FIELD_LABELS[i]), g_tfLabel, D2D1::RectF(X, yy, X + 110, yy + 26), gray);
        D2D1_RECT_F vr = D2D1::RectF(X + 120, yy, W, yy + 26);
        bool empty = c->f[i].empty();
        D2D1_COLOR_F vc = empty ? Rgb(150, 150, 150, 0.6f) : (i == F_PHONE || i == F_EMAIL ? Rgb(10, 132, 255) : text);
        Text(empty ? std::wstring(Tr(L"adicionar")) : c->f[i], g_tfValue, vr, vc);
        g_hits.push_back({ 2, i, vr });
        yy += 44;
    }
    EndPaint();
}

void EditField(int fld) {
    Contact* c = Find(g_sel);
    if (!c) return;
    for (auto& h : g_hits) if (h.kind == 2 && h.arg == fld) {
        int id = c->id;
        FieldOpen(h.r, c->f[fld], fld == F_NAME ? 24 : 14, fld == F_NAME, [id, fld](const std::wstring& t, int key) {
            if (key == VK_ESCAPE) return;
            if (Contact* x = Find(id)) { x->f[fld] = t; Save(); }
            if (key == VK_TAB && fld + 1 < F_COUNT) PostMessageW(wnd, WM_APP + 11, fld + 1, 0);  // Tab: o próximo campo
        });
        return;
    }
}
void NewContact() {
    Contact c{ g_nextId++ };
    g_c.push_back(c);
    g_sel = c.id; g_query.clear();
    Save();
    Paint();
    EditField(F_NAME);
}

// ---- vCard
std::wstring VUnesc(const std::wstring& s) { std::wstring o; for (size_t i = 0; i < s.size(); i++) { if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t n = s[++i]; o += n == L'n' || n == L'N' ? L' ' : n; } else o += s[i]; } return o; }
std::wstring PickFile(bool save) {
    IFileDialog* d = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return L"";
    COMDLG_FILTERSPEC f[] = { { L"vCard (.vcf)", L"*.vcf" } };
    d->SetFileTypes(1, f);
    if (save) { d->SetDefaultExtension(L"vcf"); d->SetFileName(Tr(L"Contatos")); }
    std::wstring path;
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); } it->Release(); } }
    d->Release();
    return path;
}
void ImportVcf() {
    std::wstring path = PickFile(false);
    if (path.empty()) return;
    auto lines = ReadLines(path);
    std::vector<std::wstring> un;
    for (auto& l : lines) { if (!l.empty() && (l[0] == L' ' || l[0] == L'\t') && !un.empty()) un.back() += l.substr(1); else un.push_back(l); }
    int n = 0; Contact cur{}; bool in = false;
    for (auto& l : un) {
        size_t c = l.find(L':'); if (c == std::wstring::npos) continue;
        std::wstring key = l.substr(0, c), val = VUnesc(l.substr(c + 1));
        std::wstring k = key.substr(0, key.find(L';')); for (auto& ch : k) ch = towupper(ch);
        if (k == L"BEGIN") { cur = Contact{}; in = true; }
        else if (k == L"END" && in) { in = false; cur.id = g_nextId++; if (!cur.f[F_NAME].empty() || !cur.f[F_PHONE].empty() || !cur.f[F_EMAIL].empty()) { g_c.push_back(cur); n++; } }
        else if (!in) continue;
        else if (k == L"FN") cur.f[F_NAME] = val;
        else if (k == L"N" && cur.f[F_NAME].empty()) { auto p = val.find(L';'); cur.f[F_NAME] = p == std::wstring::npos ? val : val.substr(p + 1, val.find(L';', p + 1) - p - 1) + L" " + val.substr(0, p); }
        else if (k == L"ORG") cur.f[F_COMPANY] = val.substr(0, val.find(L';'));
        else if (k == L"TEL" && cur.f[F_PHONE].empty()) cur.f[F_PHONE] = val;
        else if (k == L"EMAIL" && cur.f[F_EMAIL].empty()) cur.f[F_EMAIL] = val;
        else if (k == L"ADR" && cur.f[F_ADDRESS].empty()) { std::wstring a; for (wchar_t ch : val) a += ch == L';' ? L' ' : ch; while (a.find(L"  ") != std::wstring::npos) a.erase(a.find(L"  "), 1); cur.f[F_ADDRESS] = a; }
        else if (k == L"BDAY") cur.f[F_BIRTHDAY] = val;
        else if (k == L"NOTE") cur.f[F_NOTES] = val;
    }
    Save();
    wchar_t b[96]; swprintf_s(b, Tr(L"%d contatos importados."), n);
    MessageBoxW(wnd, b, Tr(L"Contatos"), MB_OK | MB_ICONINFORMATION);
}
void ExportVcf() {
    std::wstring path = PickFile(true);
    if (path.empty()) return;
    std::vector<std::wstring> out;
    auto e = [](const std::wstring& s) { std::wstring o; for (wchar_t c : s) { if (c == L',' || c == L';' || c == L'\\') o += L'\\'; if (c == L'\n') { o += L"\\n"; continue; } o += c; } return o; };
    for (auto& c : g_c) {
        out.push_back(L"BEGIN:VCARD"); out.push_back(L"VERSION:3.0");
        out.push_back(L"FN:" + e(c.f[F_NAME]));
        if (!c.f[F_COMPANY].empty()) out.push_back(L"ORG:" + e(c.f[F_COMPANY]));
        if (!c.f[F_PHONE].empty()) out.push_back(L"TEL;TYPE=CELL:" + e(c.f[F_PHONE]));
        if (!c.f[F_EMAIL].empty()) out.push_back(L"EMAIL:" + e(c.f[F_EMAIL]));
        if (!c.f[F_ADDRESS].empty()) out.push_back(L"ADR:;;" + e(c.f[F_ADDRESS]) + L";;;;");
        if (!c.f[F_BIRTHDAY].empty()) out.push_back(L"BDAY:" + e(c.f[F_BIRTHDAY]));
        if (!c.f[F_NOTES].empty()) out.push_back(L"NOTE:" + e(c.f[F_NOTES]));
        out.push_back(L"END:VCARD");
    }
    WriteLines(path, out);
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
    case WM_APP + 11: Paint(); EditField((int)w); return 0;
    case WM_MOUSEWHEEL: {
        float s = Sc(); POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        if (p.x / s < SB) { g_scroll = std::max(0.f, g_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONUP: {
        if (field.h) FieldEnd(0);
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        POINT scr{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &scr);
        for (auto& ht : g_hits) if (In(ht.r, x, y)) {
            if (ht.kind == 1) {
                g_sel = ht.arg;
                if (m == WM_RBUTTONUP) {
                    HMENU mm = CreatePopupMenu(); AppendMenuW(mm, MF_STRING, 1, Tr(L"Apagar Cartão"));
                    if (TrackPopupMenu(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x, scr.y, 0, h, nullptr) == 1) {
                        int id = ht.arg; g_c.erase(std::remove_if(g_c.begin(), g_c.end(), [id](const Contact& c) { return c.id == id; }), g_c.end()); g_sel = -1; Save();
                    }
                    DestroyMenu(mm);
                }
            } else if (ht.kind == 2) { EditField(ht.arg); return 0; }
            else if (ht.kind == 3) { NewContact(); return 0; }
            else if (ht.kind == 4) {
                FieldOpen(D2D1::RectF(ht.r.left + 28, ht.r.top, ht.r.right - 6, ht.r.bottom), g_query, 13, false, [](const std::wstring& t, int key) { if (key != VK_ESCAPE) g_query = t; else g_query.clear(); g_scroll = 0; });
                return 0;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w == 'N') NewContact();
        else if (ctrl && w == 'I') { ImportVcf(); InvalidateRect(h, nullptr, FALSE); }
        else if (ctrl && w == 'E') ExportVcf();
        else if (ctrl && w == 'F') { for (auto& ht : g_hits) if (ht.kind == 4) { PostMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM((int)((ht.r.left + 30) * Sc()), (int)((ht.r.top + 5) * Sc()))); break; } }
        else if (ctrl && w == 'W') DestroyWindow(h);
        else if (w == VK_UP || w == VK_DOWN) {
            auto v = Shown(); int i = -1; for (int k = 0; k < (int)v.size(); k++) if (v[k]->id == g_sel) i = k;
            i = std::clamp(i + (w == VK_UP ? -1 : 1), 0, (int)v.size() - 1);
            if (!v.empty()) { g_sel = v[i]->id; InvalidateRect(h, nullptr, FALSE); }
        } else if ((w == VK_DELETE) && g_sel >= 0) {
            if (MessageBoxW(h, Tr(L"Apagar este cartão?"), Tr(L"Contatos"), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                int id = g_sel; g_c.erase(std::remove_if(g_c.begin(), g_c.end(), [id](const Contact& c) { return c.id == id; }), g_c.end()); g_sel = -1; Save(); InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    g_file = DataPath(L"contatos.txt");
    Load();
    auto v = Shown(); if (!v.empty()) g_sel = v[0]->id;
    std::wstring disp = HasFont(L"SF Pro Display") ? L"SF Pro Display" : family;
    g_tfName = Fmt(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, disp.c_str());
    g_tfRow = Fmt(13);
    g_tfCenter = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfLetter = Fmt(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfLabel = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfValue = Fmt(14);
    g_tfInit = Fmt(32, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false, disp.c_str());
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfSmall = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    if (!CreateMain(hi, L"ContatosJanela", Tr(L"Contatos"), Proc, 860, 600)) return 1;
    ShowWindow(wnd, show);
    return Run();
}
