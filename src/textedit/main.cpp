// TextEdit (Mac): documento em texto formatado (.rtf) ou texto simples (.txt), com a barra de formatação do Mac
// no alto (fonte, tamanho, cor, negrito/itálico/sublinhado, alinhamento, lista). O texto é o RichEdit do Windows.
// Ctrl+N novo (outra janela), Ctrl+O abrir, Ctrl+S salvar, Ctrl+Shift+S salvar como, Ctrl+Shift+T texto simples
// / formatado, Ctrl+B/I/U, Ctrl+W fechar (pergunta se há alterações). Título: nome — Editado.
#include "../comum/app2d.h"
#include <richedit.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

using namespace app2d;

namespace {

const float BAR = 40;
HWND g_edit;
std::wstring g_path;
bool g_plain = false;
IDWriteTextFormat *g_tfBtn, *g_tfIcon, *g_tfB, *g_tfI, *g_tfU;
struct Btn { int id; D2D1_RECT_F r; };
std::vector<Btn> g_btns;
int g_hover = -1;
enum { B_FONT = 1, B_SMALLER, B_SIZE, B_BIGGER, B_COLOR, B_BOLD, B_ITALIC, B_UNDER, B_LEFT, B_CENTER, B_RIGHT, B_JUST, B_LIST };

CHARFORMAT2W CurChar() { CHARFORMAT2W cf{ sizeof(cf) }; cf.dwMask = CFM_ALL2; SendMessageW(g_edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf); return cf; }
PARAFORMAT2 CurPara() { PARAFORMAT2 pf{}; pf.cbSize = sizeof(pf); pf.dwMask = PFM_ALIGNMENT | PFM_NUMBERING; SendMessageW(g_edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf); return pf; }
void SetChar(CHARFORMAT2W& cf) { SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf); SetFocus(g_edit); InvalidateRect(wnd, nullptr, FALSE); }

void Toggle(DWORD mask, DWORD eff) {
    CHARFORMAT2W cf = CurChar();
    bool on = (cf.dwEffects & eff) && (cf.dwMask & mask);
    CHARFORMAT2W n{ sizeof(n) }; n.dwMask = mask; n.dwEffects = on ? 0 : eff;
    SetChar(n);
}
void Align(WORD a) { PARAFORMAT2 pf{}; pf.cbSize = sizeof(pf); pf.dwMask = PFM_ALIGNMENT; pf.wAlignment = a; SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf); SetFocus(g_edit); InvalidateRect(wnd, nullptr, FALSE); }
void Size(int delta) {
    CHARFORMAT2W cf = CurChar();
    int pt = cf.yHeight > 0 ? cf.yHeight / 20 : 12;
    static const int steps[] = { 9, 10, 11, 12, 13, 14, 18, 24, 36, 48, 64, 72 };
    int next = pt;
    if (delta > 0) { for (int s : steps) if (s > pt) { next = s; break; } }
    else { for (int i = 11; i >= 0; i--) if (steps[i] < pt) { next = steps[i]; break; } }
    CHARFORMAT2W n{ sizeof(n) }; n.dwMask = CFM_SIZE; n.yHeight = next * 20;
    SetChar(n);
}

void ApplyTheme() {
    bool dark = Dark();
    SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, dark ? RGB(30, 30, 30) : RGB(255, 255, 255));
    CHARFORMAT2W cf{ sizeof(cf) }; cf.dwMask = CFM_COLOR; cf.crTextColor = dark ? RGB(235, 235, 235) : RGB(20, 20, 20);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
}

void SetTitle() {
    std::wstring n = g_path.empty() ? Tr(L"Sem Título") : std::wstring(PathFindFileNameW(g_path.c_str()));
    if (SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) n += L" — " + std::wstring(Tr(L"Editado"));
    wchar_t cur[512]; GetWindowTextW(wnd, cur, 512);
    if (n != cur) SetWindowTextW(wnd, n.c_str());
}

DWORD CALLBACK ReadCb(DWORD_PTR c, LPBYTE buf, LONG n, LONG* got) { return !ReadFile((HANDLE)c, buf, n, (DWORD*)got, nullptr); }
DWORD CALLBACK WriteCb(DWORD_PTR c, LPBYTE buf, LONG n, LONG* got) { return !WriteFile((HANDLE)c, buf, n, (DWORD*)got, nullptr); }

void Layout() {
    RECT r; GetClientRect(wnd, &r);
    int top = g_plain ? 0 : (int)std::lround(BAR * Sc());
    SetWindowPos(g_edit, nullptr, 0, top, r.right, r.bottom - top, SWP_NOZORDER);
    // margens do documento, como as do TextEdit
    RECT fr{ 0, 0, r.right, r.bottom - top }; int m = (int)std::lround(22 * Sc());
    InflateRect(&fr, -m, -(int)std::lround(14 * Sc()));
    SendMessageW(g_edit, EM_SETRECT, 0, (LPARAM)&fr);
    InvalidateRect(wnd, nullptr, FALSE);
}

bool LoadFile(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::wstring x = PathFindExtensionW(p.c_str());
    g_plain = _wcsicmp(x.c_str(), L".rtf") != 0;
    // texto simples: detecta UTF-8/UTF-16 (o RichEdit lê UTF-8 com SF_USECODEPAGE)
    DWORD fmt = g_plain ? (SF_TEXT | SF_USECODEPAGE | (CP_UTF8 << 16)) : SF_RTF;
    if (g_plain) {
        BYTE bom[2] = {}; DWORD n = 0; ReadFile(f, bom, 2, &n, nullptr);
        if (n == 2 && bom[0] == 0xFF && bom[1] == 0xFE) fmt = SF_TEXT | SF_UNICODE; else SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    }
    SendMessageW(g_edit, EM_SETTEXTMODE, g_plain ? TM_PLAINTEXT : TM_RICHTEXT, 0);
    EDITSTREAM es{ (DWORD_PTR)f, 0, ReadCb };
    SendMessageW(g_edit, EM_STREAMIN, fmt, (LPARAM)&es);
    CloseHandle(f);
    g_path = p;
    ApplyTheme();
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SHAddToRecentDocs(SHARD_PATHW, p.c_str());
    Layout(); SetTitle();
    return true;
}

bool SaveAs();
bool SaveTo(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { MessageBoxW(wnd, Tr(L"Não foi possível salvar o documento."), L"TextEdit", MB_OK | MB_ICONERROR); return false; }
    bool rtf = !_wcsicmp(PathFindExtensionW(p.c_str()), L".rtf");
    if (!rtf) { BYTE bom[3] = { 0xEF, 0xBB, 0xBF }; DWORD w; WriteFile(f, bom, 3, &w, nullptr); }
    EDITSTREAM es{ (DWORD_PTR)f, 0, WriteCb };
    SendMessageW(g_edit, EM_STREAMOUT, rtf ? SF_RTF : (SF_TEXT | SF_USECODEPAGE | (CP_UTF8 << 16)), (LPARAM)&es);
    CloseHandle(f);
    g_path = p;
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SHAddToRecentDocs(SHARD_PATHW, p.c_str());
    SetTitle();
    return true;
}
bool Save() { return g_path.empty() ? SaveAs() : SaveTo(g_path); }
bool SaveAs() {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    COMDLG_FILTERSPEC rtf[] = { { L"Texto Formatado (RTF)", L"*.rtf" }, { L"Texto Simples", L"*.txt" } };
    COMDLG_FILTERSPEC txt[] = { { L"Texto Simples", L"*.txt" }, { L"Texto Formatado (RTF)", L"*.rtf" } };
    d->SetFileTypes(2, g_plain ? txt : rtf);
    d->SetDefaultExtension(g_plain ? L"txt" : L"rtf");
    d->SetFileName(g_path.empty() ? Tr(L"Sem Título") : PathFindFileNameW(g_path.c_str()));
    bool ok = false;
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { ok = SaveTo(p); CoTaskMemFree(p); } it->Release(); } }
    d->Release();
    return ok;
}
void OpenDialog() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Documentos de texto", L"*.rtf;*.txt;*.md;*.log;*.csv;*.ini;*.json" }, { L"Todos", L"*.*" } };
    d->SetFileTypes(2, f);
    if (SUCCEEDED(d->Show(wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                // documento vazio e sem nome: abre aqui; senão numa janela nova (como o Mac)
                if (g_path.empty() && !GetWindowTextLengthW(g_edit)) LoadFile(p);
                else { wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); ShellExecuteW(nullptr, L"open", me, (L"\"" + std::wstring(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}
// fechar com alterações: Salvar / Não Salvar / Cancelar
bool CanClose() {
    if (!SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) return true;
    std::wstring n = g_path.empty() ? Tr(L"Sem Título") : PathFindFileNameW(g_path.c_str());
    wchar_t q[600]; swprintf_s(q, Tr(L"Deseja salvar as alterações feitas no documento “%ls”?\n\nSuas alterações serão perdidas se você não as salvar."), n.c_str());
    int r = MessageBoxW(wnd, q, L"TextEdit", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return Save();
    return true;
}

void TogglePlain() {
    if (!g_plain) {
        if (MessageBoxW(wnd, Tr(L"Converter este documento em texto simples? Toda a formatação será perdida."), L"TextEdit", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        int n = GetWindowTextLengthW(g_edit);
        std::wstring t(n, L'\0'); GetWindowTextW(g_edit, t.data(), n + 1);
        SetWindowTextW(g_edit, L"");
        SendMessageW(g_edit, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SetWindowTextW(g_edit, t.c_str());
        g_plain = true;
    } else {
        int n = GetWindowTextLengthW(g_edit);
        std::wstring t(n, L'\0'); GetWindowTextW(g_edit, t.data(), n + 1);
        SetWindowTextW(g_edit, L"");
        SendMessageW(g_edit, EM_SETTEXTMODE, TM_RICHTEXT, 0);
        SetWindowTextW(g_edit, t.c_str());
        g_plain = false;
    }
    if (!g_path.empty()) { std::wstring p = g_path; PathRenameExtensionW(p.data(), g_plain ? L".txt" : L".rtf"); g_path = p.c_str(); }
    ApplyTheme();
    SendMessageW(g_edit, EM_SETMODIFY, TRUE, 0);
    Layout(); SetTitle();
}

void FontMenu(POINT scr) {
    const wchar_t* fonts[] = { L"Helvetica", L"SF Pro Text", L"Arial", L"Georgia", L"Times New Roman", L"Courier New", L"Menlo", L"Segoe UI", L"Verdana" };
    HMENU m = CreatePopupMenu();
    CHARFORMAT2W cf = CurChar();
    int id = 1;
    for (auto f : fonts) { if (HasFont(f)) AppendMenuW(m, MF_STRING | (!_wcsicmp(cf.szFaceName, f) ? MF_CHECKED : 0), id, f); id++; }
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c > 0) { CHARFORMAT2W n{ sizeof(n) }; n.dwMask = CFM_FACE; wcscpy_s(n.szFaceName, fonts[c - 1]); SetChar(n); }
}
void ColorMenu(POINT scr) {
    struct C { const wchar_t* n; COLORREF c; } cols[] = { { L"Automática", 0xFFFFFFFF }, { L"Preto", RGB(0, 0, 0) }, { L"Cinza", RGB(142, 142, 147) }, { L"Vermelho", RGB(255, 59, 48) },
        { L"Laranja", RGB(255, 149, 0) }, { L"Amarelo", RGB(255, 204, 0) }, { L"Verde", RGB(52, 199, 89) }, { L"Azul", RGB(0, 122, 255) }, { L"Roxo", RGB(175, 82, 222) }, { L"Rosa", RGB(255, 45, 85) } };
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 10; i++) AppendMenuW(m, MF_STRING, i + 1, Tr(cols[i].n));
    int c = TrackPopupMenu(m, TPM_RETURNCMD, scr.x, scr.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (c > 0) { CHARFORMAT2W n{ sizeof(n) }; n.dwMask = CFM_COLOR; if (cols[c - 1].c == 0xFFFFFFFF) n.dwEffects = CFE_AUTOCOLOR; else n.crTextColor = cols[c - 1].c; SetChar(n); }
}

void Action(int id, POINT scr) {
    switch (id) {
    case B_FONT: FontMenu(scr); break;
    case B_SMALLER: Size(-1); break;
    case B_BIGGER: Size(1); break;
    case B_COLOR: ColorMenu(scr); break;
    case B_BOLD: Toggle(CFM_BOLD, CFE_BOLD); break;
    case B_ITALIC: Toggle(CFM_ITALIC, CFE_ITALIC); break;
    case B_UNDER: Toggle(CFM_UNDERLINE, CFE_UNDERLINE); break;
    case B_LEFT: Align(PFA_LEFT); break;
    case B_CENTER: Align(PFA_CENTER); break;
    case B_RIGHT: Align(PFA_RIGHT); break;
    case B_JUST: Align(PFA_JUSTIFY); break;
    case B_LIST: { PARAFORMAT2 pf = CurPara(); PARAFORMAT2 n{}; n.cbSize = sizeof(n); n.dwMask = PFM_NUMBERING | PFM_OFFSET; n.wNumbering = pf.wNumbering ? 0 : PFN_BULLET; n.dxOffset = n.wNumbering ? 360 : 0;
                   SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&n); SetFocus(g_edit); InvalidateRect(wnd, nullptr, FALSE); break; }
    }
}

void Paint() {
    if (!BeginPaint()) return;
    g_btns.clear();
    bool dark = Dark();
    D2D1_SIZE_F sz = app2d::Size();
    D2D1_COLOR_F text = dark ? Rgb(235, 235, 235) : Rgb(40, 40, 42), on = Rgb(10, 132, 255);
    rt->Clear(dark ? Rgb(44, 44, 46) : Rgb(246, 246, 246));
    if (!g_plain) {
        CHARFORMAT2W cf = CurChar();
        PARAFORMAT2 pf = CurPara();
        float x = 14, y0 = 7, y1 = BAR - 7;
        auto box = [&](int id, float w, const std::wstring& label, IDWriteTextFormat* f, bool lit, bool frame) {
            D2D1_RECT_F r = D2D1::RectF(x, y0, x + w, y1);
            if (frame) Fill(r, dark ? Rgb(66, 66, 68) : Rgb(255, 255, 255), 5);
            if (lit) Fill(r, dark ? Rgb(10, 132, 255, 0.45f) : Rgb(10, 132, 255, 0.18f), 5);
            else if ((int)g_btns.size() == g_hover) Fill(r, dark ? Rgb(255, 255, 255, 0.10f) : Rgb(0, 0, 0, 0.06f), 5);
            Text(label, f, r, lit ? (dark ? Rgb(255, 255, 255) : on) : text);
            g_btns.push_back({ id, r });
            x += w + 4;
        };
        std::wstring face = cf.szFaceName[0] ? cf.szFaceName : L"Helvetica";
        box(B_FONT, 150, face + L"  ⌄", g_tfBtn, false, true);
        x += 6;
        box(B_SMALLER, 22, L"−", g_tfBtn, false, false);
        box(B_SIZE, 34, std::to_wstring(cf.yHeight > 0 ? cf.yHeight / 20 : 12), g_tfBtn, false, true);
        box(B_BIGGER, 22, L"+", g_tfBtn, false, false);
        x += 8;
        box(B_COLOR, 30, L"■", g_tfBtn, false, false);
        x += 8;
        box(B_BOLD, 28, L"B", g_tfB, (cf.dwMask & CFM_BOLD) && (cf.dwEffects & CFE_BOLD), false);
        box(B_ITALIC, 28, L"I", g_tfI, (cf.dwMask & CFM_ITALIC) && (cf.dwEffects & CFE_ITALIC), false);
        box(B_UNDER, 28, L"U", g_tfU, (cf.dwMask & CFM_UNDERLINE) && (cf.dwEffects & CFE_UNDERLINE), false);
        x += 10;
        wchar_t g1[2] = { 0xE8E4, 0 }, g2[2] = { 0xE8E3, 0 }, g3[2] = { 0xE8E2, 0 }, g4[2] = { 0xE8E2, 0 }, gl[2] = { 0xE8FD, 0 };
        box(B_LEFT, 28, g1, g_tfIcon, pf.wAlignment == PFA_LEFT, false);
        box(B_CENTER, 28, g2, g_tfIcon, pf.wAlignment == PFA_CENTER, false);
        box(B_RIGHT, 28, g3, g_tfIcon, pf.wAlignment == PFA_RIGHT, false);
        box(B_JUST, 28, L"≡", g_tfBtn, pf.wAlignment == PFA_JUSTIFY, false);
        (void)g4;
        x += 10;
        box(B_LIST, 30, gl, g_tfIcon, pf.wNumbering != 0, false);
        Fill(D2D1::RectF(0, BAR - 1, sz.width, BAR), dark ? Rgb(0, 0, 0, 0.6f) : Rgb(0, 0, 0, 0.12f));
    }
    EndPaint();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); if (g_edit) Layout(); return 0;
    case WM_DPICHANGED: OnDpi(w, l); Layout(); return 0;
    case WM_SETFOCUS: if (g_edit) SetFocus(g_edit); return 0;
    case WM_SETTINGCHANGE: ApplyTheme(); InvalidateRect(h, nullptr, FALSE); InvalidateRect(g_edit, nullptr, TRUE); return 0;
    case WM_NOTIFY: {
        auto* n = (NMHDR*)l;
        if (n->hwndFrom == g_edit && n->code == EN_SELCHANGE) InvalidateRect(h, nullptr, FALSE);  // a barra mostra o formato do cursor
        return 0;
    }
    case WM_COMMAND: if ((HWND)l == g_edit && HIWORD(w) == EN_CHANGE) SetTitle(); return 0;
    case WM_MOUSEMOVE: {
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        int hv = -1;
        for (int i = 0; i < (int)g_btns.size(); i++) if (In(g_btns[i].r, x, y)) hv = i;
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover >= 0) { g_hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        float s = Sc(), x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
        for (auto& b : g_btns) if (In(b.r, x, y)) {
            POINT scr{ (LONG)std::lround(b.r.left * s), (LONG)std::lround(b.r.bottom * s) }; ClientToScreen(h, &scr);
            Action(b.id, scr);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: if (CanClose()) DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    LoadLibraryW(L"Msftedit.dll");
    g_tfBtn = Fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfB = Fmt(14, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false, L"Georgia");
    g_tfI = nullptr; dw->CreateTextFormat(L"Georgia", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, 14, L"", &g_tfI);
    if (g_tfI) { g_tfI->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_tfI->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    g_tfU = Fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, L"Georgia");
    if (!CreateMain(hi, L"TextEditJanela", Tr(L"Sem Título"), Proc, 760, 860)) return 1;
    g_edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_WANTRETURN,
                             0, 0, 10, 10, wnd, nullptr, hi, nullptr);
    if (!g_edit) return 1;
    SendMessageW(g_edit, EM_SETEVENTMASK, 0, ENM_SELCHANGE | ENM_CHANGE);
    SendMessageW(g_edit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(g_edit, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
    {   // fonte padrão: Helvetica 12 do TextEdit (Arial no Windows, se não houver)
        CHARFORMAT2W cf{ sizeof(cf) }; cf.dwMask = CFM_FACE | CFM_SIZE; wcscpy_s(cf.szFaceName, HasFont(L"Helvetica") ? L"Helvetica" : L"Arial"); cf.yHeight = 12 * 20;
        SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }
    ApplyTheme();
    DragAcceptFiles(g_edit, FALSE);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && argv[1][0]) { if (!LoadFile(argv[1])) MessageBoxW(wnd, Tr(L"Não foi possível abrir o documento."), L"TextEdit", MB_OK | MB_ICONERROR); }
    if (argv) LocalFree(argv);
    Layout(); SetTitle();
    ShowWindow(wnd, show);
    SetFocus(g_edit);
    // atalhos (o RichEdit tem o foco: vêm pelo laço de mensagens)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0) {
            bool shift = GetKeyState(VK_SHIFT) < 0, handled = true;
            switch (msg.wParam) {
            case 'S': if (shift) SaveAs(); else Save(); break;
            case 'O': OpenDialog(); break;
            case 'N': { wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); ShellExecuteW(nullptr, L"open", me, nullptr, nullptr, SW_SHOWNORMAL); break; }
            case 'W': PostMessageW(wnd, WM_CLOSE, 0, 0); break;
            case 'B': if (!g_plain) Toggle(CFM_BOLD, CFE_BOLD); break;
            case 'I': if (!g_plain) Toggle(CFM_ITALIC, CFE_ITALIC); break;
            case 'U': if (!g_plain) Toggle(CFM_UNDERLINE, CFE_UNDERLINE); break;
            case 'T': if (shift) TogglePlain(); else handled = false; break;
            case VK_OEM_PLUS: if (!g_plain) Size(1); break;
            case VK_OEM_MINUS: if (!g_plain) Size(-1); break;
            default: handled = false;
            }
            if (handled) continue;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
