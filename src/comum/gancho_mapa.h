// Mapa compartilhado entre o Shell e a DLL carregadora (WacOSGancho.dll / WacOSGancho32.dll, src\janelas\gancho.cpp).
// O Windows guarda no máximo ~32 DLLs de gancho diferentes por login, e a WacOSWindow.dll se prende nos apps (PIN):
// cada versão nova ocupava um lugar até o app fechar, e depois de umas trocas a tabela enchia (erro 126) e as barras
// do Mac sumiam até sair e entrar no Windows (20/11/2026). Agora o gancho do Windows é sempre a carregadora, que não
// muda nunca (um lugar só, para sempre); ela lê aqui qual WacOSWindow.dll usar e a carrega com LoadLibrary, que não
// passa pela tabela de ganchos. O Shell escreve os caminhos e depois aumenta gen: a carregadora vê o gen novo e troca.
// Mesmo formato em 32 e 64 bits (só DWORD e wchar_t). Mudou o formato? Troque o nome do mapa (e o da carregadora).
#pragma once
#include <windows.h>

#define GANCHO_MAPA L"Local\\WacOSGanchoMapa1"

struct GanchoMapa {
    DWORD tamanho;            // sizeof(GanchoMapa)
    volatile LONG gen;        // muda a cada vez que o Shell escreve (a carregadora compara)
    wchar_t real64[520];      // WacOSWindow.dll (cópia em bin\obj\mj) para os apps de 64 bits
    wchar_t real32[520];      // WacOSWindow32.dll para os de 32
    wchar_t gancho32[520];    // a carregadora de 32 bits (o hospedeiro de 32 bits põe o gancho com ela)
};
