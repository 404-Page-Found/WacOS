// Modo leve: para PC fraco (Core i3 com vídeo integrado, 8 GB, HD) e para notebook na bateria (issues #10 e #33).
// Ajuste `modo_leve` (Ajustes do Sistema > Aparência > Transparência e Efeitos > Desempenho):
//   auto (padrão) = liga sozinho na bateria ou num PC com até 10 GB de memória ou até 4 núcleos lógicos;
//   1 = sempre ligado; 0 = nunca.
// Ligado:
//   - sem o vidro real (captura de até 10 janelas pela GPU e uma montagem a cada quadro que muda atrás do vidro): fica o
//     vidro da Composição, que o próprio DWM desenha;
//   - o fosco do vidro sem o desfoque extra (o fundo do DWM já vem desfocado; o gaussiano a mais por peça era o que mais
//     pesava na GPU integrada com a Central de Controle aberta, uma peça por controle), só a vibrância;
//   - as animações do Shell (ui::FrameClock) a ~60 quadros por segundo mesmo numa tela de 120/144/165 Hz.
// O resto (o desenho, os tempos das animações) não muda: elas andam por tempo real, só com menos quadros.
#include "shell.h"
#include <atomic>

namespace {
std::atomic<bool> g_ativo{ false };
bool g_lido = false;
bool g_fraco = false;     // o PC é fraco (memória/núcleos)
bool g_bateria = false;
std::wstring g_modo;
HWND g_wnd;

bool PcFraco() {
    MEMORYSTATUSEX ms{ sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
    double gb = ms.ullTotalPhys / (1024.0 * 1024 * 1024);
    // 8 GB aparece como ~7,8 (memória reservada para o vídeo integrado); 10 dá folga sem pegar os de 12/16
    return gb <= 10.0 || si.dwNumberOfProcessors <= 4;
}

bool NaBateria() {
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // tomada/bateria (só chega a janelas de nível superior, não às "só de mensagem")
    if (m == WM_POWERBROADCAST && w == PBT_APMPOWERSTATUSCHANGE) {
        if (leve::Atualizar()) backdrop::Reload();
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

bool leve::Ativo() { return g_ativo; }

bool leve::Atualizar() {
    if (!g_lido) {
        g_lido = true;
        g_fraco = PcFraco();
        MEMORYSTATUSEX ms{ sizeof(ms) }; GlobalMemoryStatusEx(&ms);
        SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
        Log(L"modo leve: PC com %.1f GB e %lu núcleos lógicos (%ls)", ms.ullTotalPhys / (1024.0 * 1024 * 1024), si.dwNumberOfProcessors,
            g_fraco ? L"fraco: o automático liga" : L"o automático só liga na bateria");
    }
    std::wstring modo = shell::Setting(L"modo_leve", L"auto");
    bool bat = NaBateria();
    bool on = modo == L"1" || (modo != L"0" && (g_fraco || bat));
    bool mudou = on != g_ativo.load();
    if (mudou || modo != g_modo || bat != g_bateria) {
        Log(L"modo leve: %ls (ajuste %ls, %ls)", on ? L"ligado" : L"desligado", modo.c_str(), bat ? L"na bateria" : L"na tomada");
    }
    g_modo = modo; g_bateria = bat;
    g_ativo = on;
    return mudou;
}

void leve::Start() {
    Atualizar();
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellModoLeve";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, shell::inst, nullptr);
}
