// Janela de Abrir/Salvar do Windows trocada pelo Finder, POR FORA, para os programas que não deixam
// a DialogoMac.dll entrar (Chrome, Discord, WhatsApp e outros que bloqueiam DLLs de terceiros).
//  1. O Windows avisa (WinEvent, fora do processo) quando um diálogo aparece; é o de arquivos se
//     tem o campo de nome (controle 1148), o botão OK e a área de itens do Explorador.
//  2. Lemos o modo (abrir/salvar/pasta), os tipos (combo 1136), o nome e a pasta atual; a janela
//     original sai da tela (continua aberta) e o Finder abre em modo escolher.
//  3. Escolheu: escrevemos o caminho no campo de nome da original e mandamos OK para ela; o
//     programa recebe o arquivo pelo caminho de sempre. Cancelou: Cancelar na original.
// Shift segurado quando a janela aparece deixa a original. Programas elevados ficam com a original.
#include "shell.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <UIAutomation.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <map>
#include <thread>

namespace {

HWINEVENTHOOK g_hook, g_objHook;
// Sem piscar: todo diálogo novo (#32770) nasce invisível (alfa 0) até sabermos o que é. Se não for
// de arquivos, volta na hora (no evento de aparecer, ou no máximo em 700 ms).
struct Veil { DWORD at; bool hadLayered; };
std::map<HWND, Veil> g_veiled;
UINT_PTR g_veilTimer;
DWORD g_selfPid;
std::mutex g_mu;
std::set<HWND> g_busy;  // diálogos sendo atendidos
std::map<HWND, RECT> g_origPos;      // onde a original estava antes de sair da tela
std::map<HWND, DWORD> g_finderTid;
std::map<HWND, HWND> g_finderWnd;    // ou a janela do Finder reserva que atende
std::atomic<bool> g_stopping{ false };  // o Shell está saindo: não responde mais nada às originais   // o Finder que atende cada um (para fechar se o Shell sair)

thread_local DWORD g_textMs = 300;  // Finish espera mais: a original pode estar ocupada (abaixo)
std::wstring Text(HWND h) {
    wchar_t buf[2048] = {};
    SendMessageTimeoutW(h, WM_GETTEXT, 2048, (LPARAM)buf, SMTO_ABORTIFHUNG, g_textMs, nullptr);
    return buf;
}

// A original responde a mensagens? Depois de trocar de pasta a janela do Photoshop/Illustrator fica alguns segundos
// ocupada (o programa trabalha a cada troca de pasta); as mensagens com 300 ms de espera desistiam em silêncio: o nome
// escolhido não entrava no campo, o OK (postado) ia mesmo assim e o arquivo saía com o nome original, ou a janela de
// escolher pasta confirmava a pasta de antes (issue #15, 22/11/2026). Espera ela ficar livre, até ms.
bool Responde(HWND dlg, DWORD ms) {
    ULONGLONG fim = GetTickCount64() + ms;
    do {
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(dlg, WM_NULL, 0, 0, SMTO_NORMAL, 250, &r)) return true;
        if (!IsWindow(dlg)) return false;
        Sleep(50);
    } while (GetTickCount64() < fim);
    return false;
}

// Escreve no campo e confere (até 5 vezes, esperando a janela ficar livre entre elas).
bool PoeTexto(HWND dlg, HWND edit, const std::wstring& v) {
    for (int k = 0; k < 5 && IsWindow(edit); k++) {
        Responde(dlg, 3000);
        SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)v.c_str(), SMTO_NORMAL, 3000, nullptr);
        if (Text(edit) == v) return true;
        Sleep(100);
    }
    return false;
}

// Controle pelo id em qualquer nível. Nas janelas antigas com modelo próprio (GetSaveFileName com
// template, ex.: o Ableton), o campo do nome (1148) e o dos tipos (1136) ficam dentro da área do
// Explorador (DUIViewWndClassName), não como filhos diretos: GetDlgItem não acha.
HWND Item(HWND dlg, int id) {
    if (HWND c = GetDlgItem(dlg, id)) return c;
    struct Find { int id; HWND found; } f{ id, nullptr };
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        auto* f = (Find*)l;
        if (GetDlgCtrlID(c) == f->id) { f->found = c; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.found;
}

// As caixas da faixa de baixo das janelas novas montadas pelo programa (ex.: o Ableton): ficam dentro de
// FloatNotifySink e sem número; a do nome tem um campo de texto (1001) dentro, a dos tipos não.
void FloatCombos(HWND dlg, HWND& nameEdit, HWND& typeCombo) {
    struct F { HWND edit, type; } f{ nullptr, nullptr };
    EnumChildWindows(dlg, [](HWND c, LPARAM l) -> BOOL {
        auto* f = (F*)l;
        wchar_t cls[32] = {}, pcls[32] = {};
        GetClassNameW(c, cls, 32);
        if (wcscmp(cls, L"ComboBox")) return TRUE;
        GetClassNameW(GetParent(c), pcls, 32);
        if (wcscmp(pcls, L"FloatNotifySink")) return TRUE;
        if (HWND e = FindWindowExW(c, nullptr, L"Edit", nullptr)) { if (!f->edit) f->edit = e; }
        else if (!f->type) f->type = c;
        return TRUE;
    }, (LPARAM)&f);
    nameEdit = f.edit; typeCombo = f.type;
}

HWND TypeCombo(HWND dlg) {
    if (HWND c = Item(dlg, 1136)) return c;
    HWND e, t; FloatCombos(dlg, e, t);
    return t;
}

// O campo de texto de verdade do "Nome do arquivo" (ComboBoxEx32 > ComboBox > Edit, ou só Edit).
HWND NameEdit(HWND dlg) {
    HWND c = Item(dlg, 1148);
    if (!c) c = Item(dlg, 1152);  // janela antiga de abrir/salvar
    if (!c) { HWND e, t; FloatCombos(dlg, e, t); return e; }  // janela nova montada pelo programa
    wchar_t cls[32]; GetClassNameW(c, cls, 32);
    if (!wcscmp(cls, L"Edit")) return c;
    if (HWND cb = FindWindowExW(c, nullptr, L"ComboBox", nullptr))
        if (HWND e = FindWindowExW(cb, nullptr, L"Edit", nullptr)) return e;
    return FindWindowExW(c, nullptr, L"Edit", nullptr);
}

bool HasChildClass(HWND parent, const wchar_t* cls) {
    struct C { const wchar_t* cls; bool found; } c{ cls, false };
    EnumChildWindows(parent, [](HWND w, LPARAM p) -> BOOL {
        auto* c = (C*)p;
        wchar_t n[64]; GetClassNameW(w, n, 64);
        if (!wcscmp(n, c->cls)) { c->found = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

bool IsFileDialog(HWND dlg) {
    wchar_t cls[32] = {}; GetClassNameW(dlg, cls, 32);
    if (wcscmp(cls, L"#32770")) return false;
    if (GetDlgItem(dlg, IDOK) && NameEdit(dlg) && (HasChildClass(dlg, L"DirectUIHWND") || HasChildClass(dlg, L"SHELLDLL_DefView"))) return true;
    // "Procurar Pasta" (SHBrowseForFolder) com campo de texto: dá para preencher
    return GetDlgItem(dlg, IDOK) && GetDlgItem(dlg, 0x3744) && HasChildClass(dlg, L"SysTreeView32");
}

HWND FolderEdit(HWND dlg) { return GetDlgItem(dlg, 0x3744); }

// marca para a barra do Mac (WacOSWindow.dll, dentro do programa) esconder o que ela desenha em volta
// da janela; o reposicionamento sem mudar nada faz ela reavaliar na hora
void Mark(HWND h, bool on) {
    if (on) SetPropW(h, L"ShellDialogoPorFora", (HANDLE)1); else RemovePropW(h, L"ShellDialogoPorFora");
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void Unveil(HWND h) {
    auto it = g_veiled.find(h);
    if (it == g_veiled.end()) return;
    if (IsWindow(h)) Mark(h, false);
    if (IsWindow(h)) {
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        if (!it->second.hadLayered) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    g_veiled.erase(it);
}

void Veil(HWND h) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool had = (ex & WS_EX_LAYERED) != 0;
    if (!had && !SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED)) return;  // programa elevado: deixa
    if (!SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA)) { if (!had) SetWindowLongPtrW(h, GWL_EXSTYLE, ex); return; }
    g_veiled[h] = { GetTickCount(), had };
    Mark(h, true);
}

void CALLBACK VeilTimer(HWND, UINT, UINT_PTR, DWORD) {  // segurança: nada fica invisível por engano
    DWORD now = GetTickCount();
    std::vector<HWND> late;
    for (auto& [h, v] : g_veiled) {
        if (!IsWindow(h)) { late.push_back(h); continue; }
        bool busy;
        { std::lock_guard<std::mutex> lk(g_mu); busy = g_busy.count(h) > 0; }
        if (!busy && now - v.at > 700) late.push_back(h);
    }
    for (HWND h : late) Unveil(h);
    if (g_veiled.empty()) { KillTimer(nullptr, g_veilTimer); g_veilTimer = 0; }
}

// Pastas especiais pelo nome que a barra de endereço mostra. Nelas a barra não traz o caminho, e sim o nome
// ("Address: Desktop", "Endereço: Área de Trabalho", "Bibliotecas\Documentos"): a pasta atual saía vazia, o Finder
// abria em Documentos em vez de onde o programa estava e a entrada da original na pasta escolhida não se confirmava
// (issue #15, 22/11/2026). Nome → caminho de todas as pastas conhecidas do Windows que têm caminho, montado uma vez.
std::wstring KnownFolderByName(const std::wstring& name) {
    static std::mutex mu;
    static std::map<std::wstring, std::wstring> byName;
    static bool built = false;
    std::lock_guard<std::mutex> lk(mu);
    if (!built) {
        built = true;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IKnownFolderManager* km = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&km))) && km) {
            KNOWNFOLDERID* ids = nullptr; UINT n = 0;
            if (SUCCEEDED(km->GetFolderIds(&ids, &n))) {
                for (UINT i = 0; i < n; i++) {
                    IKnownFolder* kf = nullptr;
                    if (FAILED(km->GetFolder(ids[i], &kf)) || !kf) continue;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(kf->GetPath(0, &path)) && path) {
                        DWORD a = GetFileAttributesW(path);
                        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                            // os nomes que a barra pode mostrar: o da pasta conhecida ("Desktop"), o da própria pasta
                            // traduzido ("Área de Trabalho", que a barra usa) e o nome no disco
                            std::vector<std::wstring> nomes{ PathFindFileNameW(path) };
                            auto nome = [&](IShellItem* it) {
                                PWSTR dn = nullptr;
                                if (it && SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) { nomes.push_back(dn); CoTaskMemFree(dn); }
                                if (it) it->Release();
                            };
                            IShellItem* it = nullptr;
                            if (SUCCEEDED(kf->GetShellItem(0, IID_PPV_ARGS(&it)))) nome(it);
                            it = nullptr;
                            if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&it)))) nome(it);
                            // o mesmo nome em duas (a Mesa do usuário e a pública): fica a do usuário
                            static const std::wstring perfil = [] { wchar_t p[MAX_PATH] = {}; GetEnvironmentVariableW(L"USERPROFILE", p, MAX_PATH); return std::wstring(p); }();
                            auto doUsuario = [&](const std::wstring& p) { return !perfil.empty() && !_wcsnicmp(p.c_str(), perfil.c_str(), perfil.size()); };
                            for (auto& n : nomes) {
                                std::wstring k = n; for (auto& ch : k) ch = towlower(ch);
                                auto [pos, novo] = byName.emplace(k, path);
                                if (!novo && !doUsuario(pos->second) && doUsuario(path)) pos->second = path;
                            }
                        }
                    }
                    if (path) CoTaskMemFree(path);
                    kf->Release();
                }
                CoTaskMemFree(ids);
            }
            km->Release();
        }
        if (SUCCEEDED(ci)) CoUninitialize();
    }
    std::wstring k = name; for (auto& ch : k) ch = towlower(ch);
    auto it = byName.find(k);
    return it == byName.end() ? L"" : it->second;
}

// Pasta onde a janela original está: a barra de endereço tem o texto "Endereço: C:\..." / "Address: ...".
// porNome: a barra só trazia o nome (pasta especial) e o caminho veio de KnownFolderByName, que pode errar quando
// duas pastas têm o mesmo nome ("Vídeos" do usuário e uma "Videos" dentro de Imagens do OneDrive).
std::wstring CurrentFolder(HWND dlg, bool* porNome = nullptr) {
    struct C { std::wstring found, name; } c;
    EnumChildWindows(dlg, [](HWND w, LPARAM p) -> BOOL {
        wchar_t cls[32]; GetClassNameW(w, cls, 32);
        if (wcscmp(cls, L"ToolbarWindow32")) return TRUE;
        std::wstring t = Text(w);
        size_t colon = t.find(L": ");
        if (colon == std::wstring::npos) return TRUE;
        std::wstring path = t.substr(colon + 2);
        DWORD a = GetFileAttributesW(path.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { ((C*)p)->found = path; return FALSE; }
        if (((C*)p)->name.empty() && !path.empty()) ((C*)p)->name = path;  // pasta especial: só o nome
        return TRUE;
    }, (LPARAM)&c);
    if (c.found.empty() && !c.name.empty()) {
        std::wstring n = c.name;
        if (size_t s = n.find_last_of(L'\\'); s != std::wstring::npos) n = n.substr(s + 1);  // "Bibliotecas\Documentos"
        c.found = KnownFolderByName(n);
        if (porNome) *porNome = true;
    }
    return c.found;
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    std::wstring x = a, y = b;
    while (x.size() > 3 && x.back() == L'\\') x.pop_back();
    while (y.size() > 3 && y.back() == L'\\') y.pop_back();
    return !_wcsicmp(x.c_str(), y.c_str());
}

struct Request {
    HWND dlg, owner;
    int mode = 0;  // 0 abrir, 1 salvar, 2 pasta
    std::vector<std::pair<std::wstring, std::wstring>> types;
    int type = 0;
    std::wstring name, folder;
};

Request Read(HWND dlg) {
    Request r;
    r.dlg = dlg;
    r.owner = GetWindow(dlg, GW_OWNER);
    std::wstring ok = Text(GetDlgItem(dlg, IDOK));
    for (auto& ch : ok) ch = towlower(ch);
    HWND combo = TypeCombo(dlg);
    if (ok.find(L"salv") != std::wstring::npos || ok.find(L"save") != std::wstring::npos) r.mode = 1;
    else if (ok.find(L"pasta") != std::wstring::npos || ok.find(L"folder") != std::wstring::npos || !combo) r.mode = 2;
    if (combo) {  // "Imagens (*.png;*.jpg)" → descrição + padrões
        int n = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < n && i < 64; i++) {
            wchar_t buf[512] = {};
            if (SendMessageW(combo, CB_GETLBTEXTLEN, i, 0) >= 511) continue;
            SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)buf);
            std::wstring t = buf, desc = t, spec = L"*.*";
            size_t o = t.rfind(L'('), c = t.rfind(L')');
            if (o != std::wstring::npos && c != std::wstring::npos && c > o) {
                spec = t.substr(o + 1, c - o - 1);
                desc = t.substr(0, o);
                while (!desc.empty() && desc.back() == L' ') desc.pop_back();
            }
            for (auto& ch : spec) if (ch == L',' ) ch = L';';
            r.types.push_back({ desc.empty() ? t : desc, spec });
        }
        r.type = std::max(0, (int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }
    // Salvar pela estrutura, não só pelo texto do botão: a janela de Abrir do Windows sempre tem o campo do nome 1148
    // (ComboBoxEx32); a de Salvar não tem, o nome fica num Edit 1001 dentro de FloatNotifySink. O botão nasce com o
    // texto do modelo ("Abrir") e o programa troca depois; lido cedo, um "Salvar como" do navegador abria o Finder
    // em modo Abrir (issue #8, 21/11/2026), e em outros idiomas ("Guardar", "Speichern") a palavra não batia.
    if (r.mode == 0 && !Item(dlg, 1148) && !Item(dlg, 1152)) { HWND e, t; FloatCombos(dlg, e, t); if (e) r.mode = 1; }
    if (!NameEdit(dlg) && FolderEdit(dlg)) r.mode = 2;
    r.name = NameEdit(dlg) ? Text(NameEdit(dlg)) : L"";
    r.folder = CurrentFolder(dlg);
    return r;
}

std::mutex g_lastMu;
std::wstring g_lastSaved;
ULONGLONG g_lastSavedAt = 0;

void Finish(const Request& r, const std::vector<std::wstring>& paths, int type) {
    if (!IsWindow(r.dlg)) return;  // o programa já fechou a janela
    if (paths.empty()) { PostMessageW(r.dlg, WM_COMMAND, IDCANCEL, 0); return; }
    g_textMs = 2000;
    Responde(r.dlg, 10000);
    if (r.mode == 1) { std::lock_guard<std::mutex> lk(g_lastMu); g_lastSaved = paths[0]; g_lastSavedAt = GetTickCount64(); }
    if (HWND combo = TypeCombo(r.dlg); combo && type >= 0 && type != r.type) {
        SendMessageTimeoutW(combo, CB_SETCURSEL, type, 0, SMTO_NORMAL, 3000, nullptr);
        SendMessageTimeoutW(r.dlg, WM_COMMAND, MAKEWPARAM(1136, CBN_SELCHANGE), (LPARAM)combo, SMTO_NORMAL, 3000, nullptr);
    }
    HWND edit = NameEdit(r.dlg);
    if (!edit) edit = FolderEdit(r.dlg);
    std::wstring value = paths[0];
    if (paths.size() > 1) {  // vários: entra na pasta e depois lista os nomes entre aspas
        std::wstring dir = paths[0].substr(0, paths[0].find_last_of(L'\\'));
        PoeTexto(r.dlg, edit, dir);
        SendMessageTimeoutW(r.dlg, WM_COMMAND, IDOK, 0, SMTO_NORMAL, 10000, nullptr);
        value.clear();
        for (auto& p : paths) value += L"\"" + std::wstring(PathFindFileNameW(p.c_str())) + L"\" ";
    } else if (r.mode != 2 && edit == NameEdit(r.dlg)) {
        // Um só: a original entra antes na pasta escolhida e recebe só o nome. Com o caminho inteiro no campo ela
        // salva certo, mas continua "na" pasta onde abriu, e programas que montam o destino com GetFolder +
        // GetFileName (Photoshop, Figma: issue #15, 21/11/2026) gravavam na pasta inicial, não na escolhida.
        size_t cut = paths[0].find_last_of(L'\\');
        std::wstring dir = cut == std::wstring::npos ? L"" : paths[0].substr(0, cut);
        if (dir.size() == 2) dir += L'\\';
        // Já "na" pasta escolhida só vale com o caminho lido na barra: pelo nome (pasta especial) pode ser outra de
        // mesmo nome, e aí o arquivo ia para a pasta errada com o nome errado (ScreenRec: salvava uma pasta acima,
        // com o nome original; issue #8, 23/11/2026). Entrar de novo na mesma pasta não faz mal.
        bool porNome = false;
        std::wstring atual = CurrentFolder(r.dlg, &porNome);
        bool jaNela = !porNome && SameFolder(atual, dir);
        Log(L"  original em '%ls'%ls; escolhida '%ls'", atual.c_str(), porNome ? L" (pelo nome)" : L"", dir.c_str());
        if (jaNela) value = PathFindFileNameW(paths[0].c_str());
        if (!dir.empty() && !jaNela) {
            PoeTexto(r.dlg, edit, dir);
            SendMessageTimeoutW(r.dlg, WM_COMMAND, IDOK, (LPARAM)GetDlgItem(r.dlg, IDOK), SMTO_NORMAL, 10000, nullptr);
            bool entrou = false;
            ULONGLONG ate = GetTickCount64() + 10000;  // pasta do OneDrive/rede e programa ocupado podem demorar
            while (IsWindow(r.dlg) && !entrou && GetTickCount64() < ate) { Sleep(40); if (Responde(r.dlg, 2000)) entrou = SameFolder(CurrentFolder(r.dlg), dir); }
            if (!IsWindow(r.dlg)) return;
            if (entrou) value = PathFindFileNameW(paths[0].c_str());  // sem confirmar a pasta, segue com o caminho inteiro
            Log(L"  original na pasta escolhida: %ls (está em '%ls')", entrou ? L"sim" : L"não (vai o caminho inteiro)", CurrentFolder(r.dlg).c_str());
        }
    }
    // a janela pode refazer o campo do nome ao trocar de pasta: o de antes já não vale
    Responde(r.dlg, 10000);
    if (HWND e2 = NameEdit(r.dlg); e2 && e2 != edit && r.mode != 2) { Log(L"  o campo do nome mudou ao trocar de pasta"); edit = e2; }
    // Só o nome: confere de novo que a original continua na pasta escolhida (o programa pode trocar a pasta ao mudar o
    // tipo ou ao recriar o campo). Se saiu, vai o caminho inteiro.
    if (r.mode == 1 && paths.size() == 1 && value.find(L'\\') == std::wstring::npos) {
        std::wstring dir = paths[0].substr(0, paths[0].find_last_of(L'\\'));
        if (dir.size() == 2) dir += L'\\';
        std::wstring atual = CurrentFolder(r.dlg);
        if (!SameFolder(atual, dir)) { Log(L"  a original saiu da pasta escolhida ('%ls'): vai o caminho inteiro", atual.c_str()); value = paths[0]; }
    }
    bool nomeOk = PoeTexto(r.dlg, edit, value);
    if (!nomeOk) {  // o campo pode ter sido refeito: procura de novo e tenta mais uma vez
        Responde(r.dlg, 5000);
        if (HWND e3 = r.mode != 2 ? NameEdit(r.dlg) : nullptr; e3) edit = e3;
        nomeOk = PoeTexto(r.dlg, edit, value);
    }
    if (!nomeOk) Log(L"  o campo do nome não ficou com o escolhido ('%ls')", Text(edit).c_str());
    PostMessageW(r.dlg, WM_COMMAND, IDOK, (LPARAM)GetDlgItem(r.dlg, IDOK));
    // A original (fora da tela) pode abrir uma pergunta depois do OK: "já existe, substituir?" ou um aviso de nome
    // inválido, que nascia fora da tela junto: o programa ficava travado até alguém apertar Esc (Firefox, issue #8,
    // 21/11/2026). Caixas de mensagem (#32770) viram o alerta do Mac no meio da tela (alertas.cpp, que agora não usa
    // mais o lugar da original quando ela está fora da tela); qualquer outra vem para o meio da tela, à frente.
    bool reenviou = false;
    for (int i = 0; i < 60 && IsWindow(r.dlg); i++) {
        Sleep(50);
        HWND pop = GetWindow(r.dlg, GW_ENABLEDPOPUP);
        if (!pop || pop == r.dlg) {
            // escolher pasta (VS Code "Abrir Pasta", issue #8): com o caminho de uma pasta no campo, o OK da original
            // entra nela em vez de devolvê-la e a janela continuava aberta fora da tela (o programa não fazia nada).
            // Já dentro da pasta, o campo vazio + OK escolhe a pasta atual. Só depois de ela estar MESMO na pasta
            // escolhida: com 400 ms fixos, numa pasta que demora a abrir (OneDrive, rede, PC mais lento) o OK ia antes
            // e o programa recebia a pasta onde a janela abriu (Illustrator, Figma, Photoshop: issue #15, 22/11/2026).
            if (r.mode == 2 && !reenviou && i >= 4 && Responde(r.dlg, 5000) && (SameFolder(CurrentFolder(r.dlg), paths[0]) || i >= 50)) {
                reenviou = true;
                Log(L"  a original entrou na pasta: confirma a pasta atual (%ls)", SameFolder(CurrentFolder(r.dlg), paths[0]) ? L"a escolhida" : L"não confirmada");
                if (edit) PoeTexto(r.dlg, edit, L"");
                PostMessageW(r.dlg, WM_COMMAND, IDOK, (LPARAM)GetDlgItem(r.dlg, IDOK));
            }
            continue;
        }
        wchar_t pcls[16] = {}; GetClassNameW(pop, pcls, 16);
        if (!wcscmp(pcls, L"#32770") && shell::Setting(L"alertas_mac", L"1") != L"0") { Log(L"  a original perguntou algo: vai para o alerta do Mac"); break; }
        RECT pr; GetWindowRect(pop, &pr);
        HMONITOR mon = MonitorFromWindow(r.owner ? r.owner : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
        int w = pr.right - pr.left, h = pr.bottom - pr.top;
        SetWindowPos(pop, HWND_TOP, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 3, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        SetLayeredWindowAttributes(pop, 0, 255, LWA_ALPHA);
        SetForegroundWindow(pop);
        Log(L"  aviso da original trazido para a tela");
        break;
    }
}

// O programa aceita vários arquivos? A lista de itens da janela original diz (SelectionPattern).
// Sem isto o Finder recebia sempre "um só" e o envio de vários ficava com o primeiro.
bool AllowsMultiple(HWND dlg) {
    bool multi = false;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) && uia) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(dlg, &root)) && root) {
            VARIANT v; v.vt = VT_I4; v.lVal = UIA_ListControlTypeId;
            IUIAutomationCondition* cond = nullptr;
            uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
            IUIAutomationElementArray* arr = nullptr;
            if (cond && SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
                int n = 0; arr->get_Length(&n);
                for (int i = 0; i < n && !multi; i++) {
                    IUIAutomationElement* e = nullptr; arr->GetElement(i, &e);
                    if (!e) continue;
                    IUIAutomationSelectionPattern* sp = nullptr;
                    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&sp))) && sp) {
                        BOOL can = FALSE;
                        if (SUCCEEDED(sp->get_CurrentCanSelectMultiple(&can)) && can) multi = true;
                        sp->Release();
                    }
                    e->Release();
                }
                arr->Release();
            }
            if (cond) cond->Release();
            root->Release();
        }
        uia->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return multi;
}

// Nativo (Shell dentro do explorer): o Finder em modo escolher roda numa thread do próprio explorer, pela
// MacDialogo.dll (a mesma que troca o Abrir/Salvar dentro dos programas), sem WacOSFinder.exe. Uma thread
// só, sempre a mesma (a janela do escolher mora nela); um pedido de cada vez. -1 = indisponível.
struct PedidoEmbutido { std::wstring args; int code = -1; HANDLE done = nullptr; };
PedidoEmbutido* g_pedido;
HANDLE g_evPedido;
std::mutex g_embMu;
DWORD g_embTid;
int(WINAPI* g_escolher)(const wchar_t*);

int Embutido(HWND dlg, const std::wstring& args) {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m = LoadLibraryW(L"MacDialogo.dll");
        g_escolher = m ? (int(WINAPI*)(const wchar_t*))GetProcAddress(m, "MacEscolher") : nullptr;
        if (!g_escolher) { Log(L"MacDialogo.dll indisponível: Abrir/Salvar por fora pelo exe"); return; }
        g_evPedido = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread([] {
            g_embTid = GetCurrentThreadId();
            OleInitialize(nullptr);
            for (;;) {  // espera pedidos sem deixar de atender mensagens (a janela escondida recebe avisos do sistema)
                DWORD w = MsgWaitForMultipleObjects(1, &g_evPedido, FALSE, INFINITE, QS_ALLINPUT);
                if (w == WAIT_OBJECT_0) {
                    if (PedidoEmbutido* p = g_pedido) { p->code = g_escolher(p->args.c_str()); SetEvent(p->done); }
                    continue;
                }
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            }
        }).detach();
    });
    if (!g_escolher) return -1;
    std::lock_guard<std::mutex> lk(g_embMu);
    PedidoEmbutido p;
    p.args = args;
    p.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pedido = &p;
    SetEvent(g_evPedido);
    bool fechou = false;
    while (WaitForSingleObject(p.done, 250) == WAIT_TIMEOUT)
        if (!fechou && (!IsWindow(dlg) || g_stopping)) {  // o programa desistiu (ou o Shell está saindo): cancela
            for (HWND w = FindWindowW(L"ArquivosEscolher", nullptr); w; w = FindWindowExW(nullptr, w, L"ArquivosEscolher", nullptr))
                if (GetWindowThreadProcessId(w, nullptr) == g_embTid) PostMessageW(w, WM_CLOSE, 0, 0);
            fechou = true;
        }
    g_pedido = nullptr;
    CloseHandle(p.done);
    return p.code;
}

void LerResultado(const wchar_t* res, std::vector<std::wstring>& paths, int& type) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, res, L"r, ccs=UTF-8") || !f) return;
    wchar_t line[2048];
    while (fgetws(line, 2048, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        if (l.rfind(L"tipo=", 0) == 0) type = _wtoi(l.c_str() + 5);
        else if (!l.empty()) paths.push_back(l);
    }
    fclose(f);
}

void Serve(Request r) {
    std::wstring finder = shell::dir + L"\\" EXE_FINDER;
    wchar_t tmp[MAX_PATH], res[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"dlg", 0, res);
    // aspas do jeito que o CommandLineToArgvW do Finder entende: barras antes de aspa dobram e aspas internas são
    // escapadas. Só pôr aspas em volta quebrava tudo com uma pasta "D:\" (a barra escapava a aspa) ou um nome com
    // aspas: o Finder abria como janela comum, na pasta do usuário, sem o Salvar (issue #19, Dia Browser, 21/11/2026)
    auto q = [](const std::wstring& s) {
        std::wstring r = L"\""; size_t bs = 0;
        for (wchar_t c : s) {
            if (c == L'\\') { bs++; continue; }
            if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
            r.append(bs, L'\\'); bs = 0; r += c;
        }
        r.append(bs * 2, L'\\'); r += L'"';
        return r;
    };
    std::wstring start = r.folder;
    if (start.empty()) { PWSTR d = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &d))) { start = d; CoTaskMemFree(d); } }
    std::wstring cmd = q(finder) + L" " + q(start) + L" --lista --escolher " + (r.mode == 1 ? L"salvar" : r.mode == 2 ? L"pasta" : L"abrir");
    if (r.mode == 1) cmd += L" --sem-confirmar";  // quem confirma a substituição é a janela original (vira o alerta do Mac, alertas.cpp)
    if (r.mode == 0 && AllowsMultiple(r.dlg)) cmd += L" --multi";  // vários arquivos de uma vez (upload do navegador etc.)
    if (!r.types.empty() && r.mode != 2) {
        std::wstring t;
        for (auto& [d, sp] : r.types) { if (!t.empty()) t += L"||"; t += d + L"|" + sp; }
        cmd += L" --tipos " + q(t) + L" --tipo " + std::to_wstring(r.type);
    }
    if (!r.name.empty()) cmd += L" --nome " + q(r.name);
    cmd += L" --resultado " + q(res);
    if (r.owner) cmd += L" --pai " + std::to_wstring((ULONG_PTR)r.owner);
    Log(L"diálogo por fora: %ls", cmd.c_str());
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::vector<std::wstring> paths;
    int type = -1;
    // Direto para o Finder reserva (já aberto e escondido): sem abrir um processo só para repassar o
    // pedido a ele, a janela aparece na hora. Sem reserva, abre o Finder como antes.
    int embutido = shell::nativo ? Embutido(r.dlg, cmd.substr(q(finder).size() + 1)) : -1;  // nativo: aqui no explorer
    if (embutido >= 0) Log(L"  embutido no explorer (%d)", embutido);
    HANDLE proc = nullptr; HWND spare = shell::nativo ? nullptr : FindWindowW(L"ArquivosJanela", L"\u200BArquivosReserva");
    if (spare) {
        DWORD rpid = 0; GetWindowThreadProcessId(spare, &rpid);
        proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rpid);
        AllowSetForegroundWindow(rpid);
        COPYDATASTRUCT cd{ 0x41525156 /*'ARQV'*/, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (void*)cmd.c_str() };
        DWORD_PTR ok = 0;
        // Reserva ocupada (ainda se preparando): o pedido fica na fila dela e é atendido depois. Esgotar o tempo
        // não é recusa; antes abria outro Finder e apareciam DUAS janelas de salvar no Ableton (16/11/2026).
        if (proc) {
            SetLastError(0);
            LRESULT sent = SendMessageTimeoutW(spare, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &ok);
            bool tarde = !sent && GetLastError() == ERROR_TIMEOUT && IsWindow(spare);
            if (tarde) Log(L"  reserva do Finder demorou; o pedido segue com ela");
            else if (!sent || !ok) { CloseHandle(proc); proc = nullptr; }
        }
        if (proc) Log(L"  pela reserva do Finder");
    }
    if (embutido < 0 && !proc && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        spare = nullptr;
    }
    if (embutido >= 0) {
        if (embutido == 0) LerResultado(res, paths, type);
    } else if (proc) {
        { std::lock_guard<std::mutex> lk(g_mu); g_finderTid[r.dlg] = spare ? 0 : pi.dwThreadId; g_finderWnd[r.dlg] = spare; }
        while (WaitForSingleObject(proc, 250) == WAIT_TIMEOUT)
            if (!IsWindow(r.dlg)) {  // o programa desistiu
                if (spare) PostMessageW(spare, WM_CLOSE, 0, 0); else PostThreadMessageW(pi.dwThreadId, WM_QUIT, 0, 0);
                break;
            }
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (code == 0) {
            FILE* f = nullptr;
            if (!_wfopen_s(&f, res, L"r, ccs=UTF-8") && f) {
                wchar_t line[2048];
                while (fgetws(line, 2048, f)) {
                    std::wstring l = line;
                    while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
                    if (l.rfind(L"tipo=", 0) == 0) type = _wtoi(l.c_str() + 5);
                    else if (!l.empty()) paths.push_back(l);
                }
                fclose(f);
            }
        }
    } else {  // sem Finder: devolve a janela original para a tela
        SetWindowPos(r.dlg, nullptr, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetLayeredWindowAttributes(r.dlg, 0, 255, LWA_ALPHA);
    }
    DeleteFileW(res);
    if (g_stopping) return;  // a original já foi devolvida à tela por Stop: fica com o usuário
    Finish(r, paths, type);
    Log(L"  %ls", paths.empty() ? L"cancelado" : paths[0].c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_busy.erase(r.dlg);
    g_origPos.erase(r.dlg);
    g_finderTid.erase(r.dlg);
    g_finderWnd.erase(r.dlg);
}

void CALLBACK OnDialog(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD, DWORD);

// Janelas montadas pelo próprio programa (GetSaveFileName com modelo, ex.: o Ableton) ganham a área do
// Explorador e o campo do nome um pouco DEPOIS de aparecer: na primeira olhada ainda não são "de
// arquivos". Ficam invisíveis e são olhadas de novo a cada 15 ms por até 600 ms.
std::map<HWND, DWORD> g_recheck;
UINT_PTR g_recheckTimer;
void CALLBACK RecheckTimer(HWND, UINT, UINT_PTR, DWORD) {
    DWORD now = GetTickCount();
    std::vector<HWND> ready, gone;
    for (auto& [h, at] : g_recheck) {
        if (!IsWindow(h)) { gone.push_back(h); continue; }
        if (IsFileDialog(h)) ready.push_back(h);
        else if (now - at > 600) gone.push_back(h);
    }
    for (HWND h : gone) {
        g_recheck.erase(h);
        if (IsWindow(h)) {
            wchar_t t[80] = {}; GetWindowTextW(h, t, 80);
            Log(L"diálogos por fora: \"%ls\" não é de arquivos (nome %d, ok %d, área %d)", t, Item(h, 1148) != nullptr, GetDlgItem(h, IDOK) != nullptr,
                HasChildClass(h, L"DirectUIHWND") || HasChildClass(h, L"SHELLDLL_DefView"));
            Unveil(h);
        }
    }
    for (HWND h : ready) {
        g_recheck.erase(h);
        Log(L"diálogos por fora: reconhecida na segunda olhada");
        OnDialog(nullptr, EVENT_SYSTEM_DIALOGSTART, h, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }
    if (g_recheck.empty() && g_recheckTimer) { KillTimer(nullptr, g_recheckTimer); g_recheckTimer = 0; }
}

void CALLBACK OnDialog(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    wchar_t cls[16] = {}; GetClassNameW(hwnd, cls, 16);
    if (wcscmp(cls, L"#32770")) return;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_selfPid) return;
    // Seletor dos apps da Loja (PickerHost.exe, ex.: "Adicionar exclusão" da Segurança do Windows): a resposta
    // escrita no diálogo não volta para o app, e o original escondido deixava a janela do app desativada (não
    // dava para clicar, 09/11/2026). Fica o diálogo do Windows.
    {
        static std::map<DWORD, bool> picker;
        auto it = picker.find(pid);
        if (it == picker.end()) {
            bool is = false;
            if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(hp, 0, path, &n)) is = !_wcsicmp(PathFindFileNameW(path), L"PickerHost.exe");
                CloseHandle(hp);
            }
            if (picker.size() > 512) picker.clear();
            it = picker.emplace(pid, is).first;
        }
        if (it->second) return;
    }
    if (ev == EVENT_OBJECT_CREATE) {  // nasce invisível até sabermos o que é
        if (GetKeyState(VK_SHIFT) & 0x8000) return;
        Veil(hwnd);
        if (!g_veilTimer) g_veilTimer = SetTimer(nullptr, 0, 120, VeilTimer);
        return;
    }
    if (!IsFileDialog(hwnd)) {
        // tem os controles do modelo das janelas de arquivos (lista 1120, textos 1091...): ainda montando, olha
        // de novo; caixas de mensagem e outros diálogos não esperam
        if (GetDlgItem(hwnd, IDOK) && (GetDlgItem(hwnd, 1120) || GetDlgItem(hwnd, 1091) || HasChildClass(hwnd, L"DUIViewWndClassName")) && !g_recheck.count(hwnd) && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            if (!g_veiled.count(hwnd)) Veil(hwnd);
            if (auto v = g_veiled.find(hwnd); v != g_veiled.end()) v->second.at = GetTickCount();  // o VeilTimer não solta antes
            g_recheck[hwnd] = GetTickCount();
            if (!g_recheckTimer) g_recheckTimer = SetTimer(nullptr, 0, 15, RecheckTimer);
            return;
        }
        if (!g_recheck.count(hwnd)) Unveil(hwnd);
        return;
    }
    if (GetKeyState(VK_SHIFT) & 0x8000) { Unveil(hwnd); return; }  // escape: a janela original
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_busy.count(hwnd)) return;
        g_busy.insert(hwnd);
    }
    Request r = Read(hwnd);
    Mark(hwnd, true);  // a barra do Mac do programa some com ela
    RECT orig{}; GetWindowRect(hwnd, &orig);
    if (orig.left > -30000) { std::lock_guard<std::mutex> lk(g_mu); g_origPos[hwnd] = orig; }
    // tira a original da tela; se não der (programa elevado), ela fica
    if (!SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        { std::lock_guard<std::mutex> lk(g_mu); g_busy.erase(hwnd); }
        Unveil(hwnd);
        return;
    }
    std::thread(Serve, r).detach();
}

}  // namespace

std::wstring dialogs::LastSaved() {
    std::lock_guard<std::mutex> lk(g_lastMu);
    return GetTickCount64() - g_lastSavedAt < 20000 ? g_lastSaved : L"";
}

void dialogs::Start() {
    if (shell::Setting(L"dialogo_por_fora", L"1") != L"1") return;
    g_selfPid = GetCurrentProcessId();
    g_hook = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART, nullptr, OnDialog, 0, 0,
                             WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    g_objHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnDialog, 0, 0,
                                WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    Log(L"diálogos por fora: %ls", g_hook ? L"ok" : L"falhou");
    // janelas de abrir/salvar que já estavam abertas (abertas enquanto o Shell reiniciava, ou deixadas fora
    // da tela por um Shell que caiu no meio): o Finder atende agora
    std::vector<HWND> open;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != g_selfPid && IsWindowVisible(h) && IsFileDialog(h)) ((std::vector<HWND>*)l)->push_back(h);
        return TRUE;
    }, (LPARAM)&open);
    for (HWND h : open) {
        RECT r; GetWindowRect(h, &r);
        Log(L"diálogos por fora: janela de arquivos já aberta%ls", r.left <= -30000 ? L" (estava fora da tela)" : L"");
        OnDialog(nullptr, EVENT_SYSTEM_DIALOGSTART, h, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }
}

void dialogs::Stop() {
    g_stopping = true;
    if (g_hook) UnhookWinEvent(g_hook);
    if (g_objHook) UnhookWinEvent(g_objHook);
    g_hook = g_objHook = nullptr;
    std::vector<HWND> all;
    for (auto& [h, v] : g_veiled) all.push_back(h);
    for (HWND h : all) Unveil(h);  // nada fica invisível quando o shell sai
    // as que estavam sendo atendidas voltam para a tela (senão o app fica com uma janela invisível e
    // parece travado) e o Finder delas fecha
    std::lock_guard<std::mutex> lk(g_mu);
    for (HWND h : g_busy) {
        if (!IsWindow(h)) continue;
        auto p = g_origPos.find(h);
        RECT r = p != g_origPos.end() ? p->second : RECT{ 100, 100, 100, 100 };
        SetWindowPos(h, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        auto f = g_finderTid.find(h);
        if (f != g_finderTid.end() && f->second) PostThreadMessageW(f->second, WM_QUIT, 0, 0);
        auto fw = g_finderWnd.find(h);
        if (fw != g_finderWnd.end() && fw->second) PostMessageW(fw->second, WM_CLOSE, 0, 0);
    }
}
