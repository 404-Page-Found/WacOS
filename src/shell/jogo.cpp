// Modo Jogo (como o Game Mode do macOS): quando um jogo vem para a frente, o Shell desliga o que mais pesa nele e
// devolve tudo quando o jogo fecha. Medido em 20/11/2026 com o Fortnite aberto: o Shell ia a ~50% de um núcleo,
// 4,5% da GPU e 514 MB, por causa de:
//   - vidro real: remontava o fundo a cada quadro do jogo atrás das peças (335 montagens e 1255 redesenhos em 10 s);
//   - "sacudir para localizar" (ponteiro.cpp): Raw Input de TODO movimento do mouse (mouse de 1000 Hz = 1000/s);
//   - "clique mostra a mesa" (revelar.cpp): gancho global de mouse (cada clique do jogo passa pelo Shell);
//   - Cantos Ativos: leitura do ponteiro 20 vezes por segundo;
//   - a mesa recriada (papel de parede 5K decodificado de novo) a cada troca de modo de tela do jogo (mesa.cpp).
// No modo jogo: vidro real desligado (fica o vidro comum), sem Raw Input, sem gancho de mouse, sem Cantos Ativos,
// a mesa só se refaz depois, e o Shell em prioridade abaixo do normal e no modo eficiência (EcoQoS: núcleos E).
// O teclado (Win, Alt+Tab do Mac) continua. Ajuste modo_jogo (1 liga, padrão). Jogos a mais: %APPDATA%\Shell\jogos.txt
// (um .exe ou parte do caminho por linha). Não abre o processo do jogo além do nome do .exe (anti-cheat).
#include "shell.h"
#include <shlobj.h>
#include <shellapi.h>
#include <atomic>

namespace {

std::atomic<bool> g_on{ false };  // lido também pela thread do índice
bool g_enabled = true;
DWORD g_pid = 0;
int g_gone = 0;                 // verificações seguidas sem janela do jogo
std::wstring g_name;
std::vector<std::wstring> g_extra;

// motores e bibliotecas de jogos (classe da janela principal)
const wchar_t* CLASSES[] = { L"UnrealWindow", L"UnityWndClass", L"SDL_app", L"GLFW30", L"Valve001", L"CryENGINE", L"RiotWindowClass",
                             L"LWJGL", L"SFML_Window", L"OgreD3D11Wnd", L"TankWindowClass" };
// pastas de jogos das lojas
const wchar_t* PATHS[] = { L"\\steamapps\\common\\", L"\\epic games\\", L"\\riot games\\", L"\\xboxgames\\", L"\\ea games\\",
                           L"\\electronic arts\\", L"\\ubisoft game launcher\\games\\", L"\\gog galaxy\\games\\", L"\\rockstar games\\",
                           L"\\call of duty\\", L"\\overwatch\\" };
// o que fica em tela cheia e não é jogo (vídeo, apresentação, navegador)
const wchar_t* NOT_GAMES[] = { L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"brave.exe", L"vivaldi.exe", L"vlc.exe",
                               L"mpc-hc64.exe", L"mpc-be64.exe", L"mpv.exe", L"potplayermini64.exe", L"powerpnt.exe", L"obs64.exe",
                               L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"explorer.exe", L"applicationframehost.exe",
                               // os launchers não são jogo: o Riot Client abre no login e fica na frente, e o modo jogo desligava o
                               // vidro real até reiniciar (22/11/2026). O jogo que ele abre é pego pelo motor ou pela tela cheia.
                               L"riot client.exe", L"riotclientux.exe", L"riotclientuxrender.exe", L"riotclientservices.exe",
                               L"eadesktop.exe", L"ealauncher.exe", L"upc.exe", L"ubisoftconnect.exe", L"battle.net.exe",
                               L"galaxyclient.exe", L"rockstarlauncher.exe", L"epicwebhelper.exe", L"hoyoplay.exe" };

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

void LoadExtra() {
    g_extra.clear();
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) return;
    std::wstring f = std::wstring(p) + L"\\Shell\\jogos.txt";
    CoTaskMemFree(p);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"r, ccs=UTF-8") || !fp) return;
    wchar_t line[512];
    while (fgetws(line, 512, fp)) {
        std::wstring s = line;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        if (!s.empty() && s[0] != L'#') g_extra.push_back(Lower(s));
    }
    fclose(fp);
}

// caminho do .exe só com o direito mínimo (o mesmo que o Gerenciador de Tarefas usa)
std::wstring ExePath(DWORD pid) {
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return L"";
    wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(ph, 0, path, &n);
    CloseHandle(ph);
    return Lower(path);
}

bool CoversMonitor(HWND h) {
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT r; if (!GetWindowRect(h, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// a janela da frente é de um jogo? (por que: para o log)
bool IsGame(HWND fg, DWORD pid, std::wstring& exe, const wchar_t*& why) {
    if (!fg || !pid || pid == GetCurrentProcessId() || apps::IsShellWindow(fg)) return false;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    std::wstring path = ExePath(pid);
    exe = path.substr(path.find_last_of(L'\\') + 1);
    if (exe.empty() || exe.find(L"wacos") != std::wstring::npos) return false;
    for (auto n : NOT_GAMES) if (exe == n) return false;
    for (auto& e : g_extra) if (exe == e || path.find(e) != std::wstring::npos) { why = L"jogos.txt"; return true; }
    for (auto c : CLASSES) if (!wcscmp(cls, c)) { why = L"motor de jogo"; return true; }
    for (auto p : PATHS) if (path.find(p) != std::wstring::npos) { why = L"pasta de jogos"; return true; }
    // Direct3D em tela cheia exclusiva (o Windows sabe): jogo
    QUERY_USER_NOTIFICATION_STATE q{};
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN && CoversMonitor(fg)) { why = L"Direct3D em tela cheia"; return true; }
    return false;
}

bool HasWindow(DWORD pid) {
    struct C { DWORD pid; bool found; } c{ pid, false };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = (C*)l;
        DWORD p = 0; GetWindowThreadProcessId(h, &p);
        if (p == c->pid && IsWindowVisible(h)) { c->found = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

// Ganchos de EVENT_OBJECT_LOCATIONCHANGE (tela cheia, notificações, sombras, vidro): o Windows não deixa filtrar o
// objeto, então cada movimento do CURSOR também chega ao Shell, uma vez por gancho. Num jogo (o cursor é recentrado a
// cada quadro, mouse de 1000 Hz) eram milhares de avisos por segundo na thread principal; medido 20/11/2026, com o
// vidro real já desligado: 43% de um núcleo logo depois de abrir. No modo jogo eles saem e voltam quando o jogo fecha.
// Fora do modo jogo também pesava: eram 4 ganchos (vidro, tela cheia, notificações, sombras), então o Windows entregava
// cada movimento do cursor (e de cada barra de rolagem, cursor de texto, controle filho de qualquer app) 4 vezes à
// thread da interface, de processo para processo. Agora é UM gancho só, que joga fora na hora o que não é janela
// (todos os que pedem só querem OBJID_WINDOW) e repassa o resto a cada um (23/11/2026, otimização de desempenho).
struct Gancho { WINEVENTPROC fn; DWORD flags; };
std::vector<Gancho> g_ganchos;
HWINEVENTHOOK g_posicao;       // o gancho único (nullptr no modo jogo)
DWORD g_posicaoFlags;
unsigned g_avisos, g_deJanela;  // contagem para o log (Tick)
void CALLBACK Posicao(HWINEVENTHOOK hk, DWORD ev, HWND h, LONG obj, LONG child, DWORD tid, DWORD t) {
    g_avisos++;
    if (obj != OBJID_WINDOW || !h) return;  // cursor, cursor de texto, barras de rolagem: ninguém usa
    g_deJanela++;
    static const DWORD eu = GetCurrentProcessId();
    int proprio = -1;
    auto lista = g_ganchos;  // quem é chamado pode soltar o próprio gancho (SoltarPosicao)
    for (auto& g : lista) {
        if ((g.flags & WINEVENT_SKIPOWNPROCESS) && !(g_posicaoFlags & WINEVENT_SKIPOWNPROCESS)) {  // este não quer as do Shell
            if (proprio < 0) { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); proprio = pid == eu; }
            if (proprio) continue;
        }
        g.fn(hk, ev, h, obj, child, tid, t);
    }
}
void Ganchos(bool on) {
    if (g_posicao) { UnhookWinEvent(g_posicao); g_posicao = nullptr; }
    if (!on || g_ganchos.empty()) return;
    // todos dispensam as janelas do próprio Shell (o normal): o Windows já filtra e o Dock animando não chega aqui
    bool todosSemProprio = true;
    for (auto& g : g_ganchos) if (!(g.flags & WINEVENT_SKIPOWNPROCESS)) todosSemProprio = false;
    g_posicaoFlags = WINEVENT_OUTOFCONTEXT | (todosSemProprio ? WINEVENT_SKIPOWNPROCESS : 0);
    g_posicao = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, Posicao, 0, 0, g_posicaoFlags);
}

void Eco(bool on) {
    SetPriorityClass(GetCurrentProcess(), on ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                                      on ? (ULONG)PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0UL };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}

void Apply(bool on) {
    g_on = on;
    Log(on ? L"modo jogo: ligado (%ls, pid %lu)" : L"modo jogo: desligado (%ls fechou)", g_name.c_str(), g_pid);
    backdrop::Reload();      // vidro real fora (backdrop.cpp confere jogo::Ativo)
    ponteiro::Pausar(on);    // sem Raw Input do mouse
    revelar::Pausar(on);     // sem gancho global do mouse
    cantos::Pausar(on);      // sem ler o ponteiro 20x/s
    Ganchos(!on);            // sem os avisos de posição (cada movimento do cursor)
    Eco(on);
    if (!on) mesa::DepoisDoJogo();  // a troca de tela que ficou para depois
}

void Check(HWND fg) {
    if (!g_enabled) return;
    if (g_on) {
        if (HasWindow(g_pid)) { g_gone = 0; return; }
        if (++g_gone < 2) return;  // ~4 s sem janela nenhuma: o jogo fechou
        Apply(false);
        g_pid = 0; g_gone = 0;
        return;
    }
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    std::wstring exe; const wchar_t* why = L"";
    if (!IsGame(fg, pid, exe, why)) return;
    g_pid = pid; g_name = exe; g_gone = 0;
    Log(L"modo jogo: %ls detectado (%ls)", exe.c_str(), why);
    Apply(true);
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    Check(GetForegroundWindow());
    static int n = 0;
    if (++n % 30 == 0 && g_avisos) {  // a cada minuto (diagnóstico de CPU): quantos avisos de posição chegaram
        Log(L"posição: %u avisos em 60 s, %u de janela repassados a %zu (antes cada um chegava %zu vezes)", g_avisos, g_deJanela, g_ganchos.size(), g_ganchos.size());
        g_avisos = g_deJanela = 0;
    }
}

}  // namespace

void jogo::Start() {
    Reload();
    SetTimer(nullptr, 0, 2000, Tick);
}

void jogo::Reload() {
    bool was = g_enabled;
    g_enabled = shell::Setting(L"modo_jogo", L"1") != L"0";
    LoadExtra();
    if (was && !g_enabled && g_on) { Apply(false); g_pid = 0; }
}

void jogo::Foreground(HWND h) { Check(h); }

// no lugar de SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE...): o mesmo gancho, que sai durante o modo jogo
void jogo::GanchoPosicao(WINEVENTPROC fn, DWORD flags) {
    g_ganchos.push_back({ fn, flags });
    Ganchos(!g_on);  // refaz o gancho único (as opções podem mudar com o novo)
}
void jogo::SoltarPosicao(WINEVENTPROC fn) {
    for (size_t i = 0; i < g_ganchos.size(); i++)
        if (g_ganchos[i].fn == fn) { g_ganchos.erase(g_ganchos.begin() + i); Ganchos(!g_on); return; }
}
bool jogo::Ativo() { return g_on; }
