// Ditado (o microfone da Siri e do Spotlight, pedido do usuário em 23/11/2026): o reconhecimento de fala do próprio
// Windows (Windows.Media.SpeechRecognition), numa thread. Uma frase por vez: o texto parcial chega enquanto a pessoa fala
// (HypothesisGenerated) e o final quando ela para (1,2 s de silêncio). Idioma: o do WacOS, se o Windows tiver; senão o de
// fala do Windows. Precisa do "Reconhecimento de fala online" ligado (Privacidade > Fala); desligado, avisa (V_PRIVACIDADE).
// Mensagens para a janela que pediu: wParam = voz::Tipo, lParam = new std::wstring (quem recebe apaga).
#include "shell.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>
#include <thread>
#include <mutex>
#include <atomic>
#pragma comment(lib, "windowsapp.lib")

namespace sr = winrt::Windows::Media::SpeechRecognition;

namespace {
std::mutex g_mx;
winrt::Windows::Foundation::IAsyncOperation<sr::SpeechRecognitionResult> g_op{ nullptr };
std::atomic<int> g_gen{ 0 };
std::atomic<bool> g_ativa{ false };

void Manda(HWND to, UINT msg, int gen, voz::Tipo t, const std::wstring& s) {
    if (gen != g_gen) return;  // de uma escuta que já foi parada
    if (!PostMessageW(to, msg, (WPARAM)t, (LPARAM)new std::wstring(s))) { /* janela sumiu: o texto se perde */ }
}

// o idioma do WacOS entre os que o Windows reconhece (pt-BR, en-US...); vazio = o de fala do Windows
std::wstring IdiomaDeFala() {
    std::wstring codigo = idioma::kInfo[idioma::Current()].code;  // "pt", "en", "es"...
    try {
        for (auto const& l : sr::SpeechRecognizer::SupportedTopicLanguages()) {
            std::wstring tag = l.LanguageTag().c_str();
            if (_wcsnicmp(tag.c_str(), codigo.c_str(), codigo.size()) == 0) return tag;
        }
    } catch (...) {}
    return L"";
}
}  // namespace

bool voz::Ouvindo() { return g_ativa; }

void voz::Parar() {
    g_gen++;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_op) { try { g_op.Cancel(); } catch (...) {} }
    g_ativa = false;
}

void voz::Ouvir(HWND to, UINT msg) {
    Parar();
    int gen = ++g_gen;
    g_ativa = true;
    std::thread([to, msg, gen] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            std::wstring tag = IdiomaDeFala();
            sr::SpeechRecognizer rec = tag.empty() ? sr::SpeechRecognizer() : sr::SpeechRecognizer(winrt::Windows::Globalization::Language(tag));
            rec.Timeouts().InitialSilenceTimeout(std::chrono::seconds(6));
            rec.Timeouts().EndSilenceTimeout(std::chrono::milliseconds(1200));
            rec.HypothesisGenerated([to, msg, gen](auto const&, sr::SpeechRecognitionHypothesisGeneratedEventArgs const& a) {
                Manda(to, msg, gen, voz::V_PARCIAL, a.Hypothesis().Text().c_str());
            });
            auto c = rec.CompileConstraintsAsync().get();
            if (c.Status() != sr::SpeechRecognitionResultStatus::Success) {
                Log(L"voz: não preparou o reconhecimento (%d, %ls)", (int)c.Status(), tag.c_str());
                Manda(to, msg, gen, voz::V_ERRO, Tr(L"O reconhecimento de fala do Windows não está disponível neste idioma."));
            } else {
                Log(L"voz: ouvindo (%ls)", tag.empty() ? L"idioma de fala do Windows" : tag.c_str());
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (gen != g_gen) { g_ativa = false; return; }
                    g_op = rec.RecognizeAsync();
                }
                auto r = g_op.get();
                std::wstring t = r.Text().c_str();
                Log(L"voz: %zu letras (estado %d)", t.size(), (int)r.Status());
                Manda(to, msg, gen, voz::V_FINAL, t);
            }
        } catch (winrt::hresult_canceled const&) {
            // parada pela pessoa (clicou no microfone de novo, fechou o painel)
        } catch (winrt::hresult_error const& e) {
            unsigned hr = (unsigned)e.code();
            Log(L"voz: erro 0x%08X %ls", hr, e.message().c_str());
            if (hr == 0x80045509) {  // SPERR_SPEECH_PRIVACY_POLICY_NOT_ACCEPTED: o reconhecimento online está desligado
                // a pessoa clicou no microfone: abre a página do Windows onde se liga (Privacidade > Fala)
                if (gen == g_gen) shell::Abrir(L"open", L"ms-settings:privacy-speech", nullptr, nullptr, SW_SHOWNORMAL);
                Manda(to, msg, gen, voz::V_PRIVACIDADE, Tr(L"Para ditar, ligue o \"Reconhecimento de fala online\" em Configurações do Windows > Privacidade > Fala."));
            } else if (hr == 0x80070005)  // sem permissão do microfone
                Manda(to, msg, gen, voz::V_ERRO, Tr(L"O Windows não deixou usar o microfone. Libere em Configurações > Privacidade > Microfone."));
            else
                Manda(to, msg, gen, voz::V_ERRO, Tr(L"Não deu para ouvir agora. Confira se há um microfone ligado."));
        } catch (...) {
            Manda(to, msg, gen, voz::V_ERRO, Tr(L"Não deu para ouvir agora. Confira se há um microfone ligado."));
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (gen == g_gen) { g_op = nullptr; g_ativa = false; }
        }
    }).detach();
}
