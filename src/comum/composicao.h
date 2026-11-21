// Teclados com composição (coreano, japonês, chinês) nos campos desenhados pelo Shell (Spotlight, Launchpad).
// O texto em composição entra direto no fim do campo (e já é buscado) e é trocado pelo resultado quando o teclado
// confirma. Antes a sílaba coreana só chegava (WM_CHAR) depois da letra seguinte e a janela de composição do Windows
// aparecia solta em cima do campo (PR #22 apontou, 21/11/2026).
//
// Uso no WndProc:
//   if (m == WM_IME_SETCONTEXT) l &= ~ISC_SHOWUICOMPOSITIONWINDOW;   // antes do DefWindowProc
//   bool mudou = false;
//   if (g_comp.Trata(h, m, l, texto, mudou)) { if (mudou) ...; return 0; }
// e g_comp.Cancela(h, texto) ao fechar o painel ou limpar o campo.
#pragma once
#include <windows.h>
#include <imm.h>
#include <string>
#include <algorithm>
#pragma comment(lib, "imm32.lib")

struct Composicao {
    std::wstring atual;  // o que está no fim do texto agora, ainda em composição

    // tira do texto a composição anterior (se o texto ainda termina com ela: o campo pode ter sido limpo no meio)
    void Tira(std::wstring& texto) {
        if (!atual.empty() && texto.size() >= atual.size() && texto.compare(texto.size() - atual.size(), atual.size(), atual) == 0)
            texto.resize(texto.size() - atual.size());
        atual.clear();
    }

    bool Trata(HWND h, UINT m, LPARAM l, std::wstring& texto, bool& mudou) {
        if (m == WM_IME_STARTCOMPOSITION) { atual.clear(); return true; }  // sem a janela de composição do Windows
        if (m == WM_IME_ENDCOMPOSITION) {
            if (!atual.empty()) { Tira(texto); mudou = true; }  // desistiu (Esc no teclado): some do campo
            return true;
        }
        if (m != WM_IME_COMPOSITION) return false;
        HIMC c = ImmGetContext(h);
        if (!c) return false;
        auto ler = [&](DWORD k) {
            std::wstring s;
            LONG n = ImmGetCompositionStringW(c, k, nullptr, 0);
            if (n > 0) { s.resize((size_t)n / sizeof(wchar_t)); ImmGetCompositionStringW(c, k, s.data(), (DWORD)n); }
            return s;
        };
        Tira(texto);
        if (l & GCS_RESULTSTR) texto += ler(GCS_RESULTSTR);  // confirmado: fica de vez (e não vem de novo como WM_CHAR)
        if (l & GCS_COMPSTR) { atual = ler(GCS_COMPSTR); texto += atual; }
        ImmReleaseContext(h, c);
        mudou = true;
        return true;
    }

    // fecha o painel ou limpa o campo: a composição pela metade é descartada (no teclado também)
    void Cancela(HWND h, std::wstring& texto) {
        if (atual.empty()) return;
        Tira(texto);
        if (HIMC c = ImmGetContext(h)) { ImmNotifyIME(c, NI_COMPOSITIONSTR, CPS_CANCEL, 0); ImmReleaseContext(h, c); }
    }
};
