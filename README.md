# shell-proprio

Shell próprio para Windows com comportamento e visual inspirados no macOS, rodando por cima
do Windows (kernel, drivers e apps continuam os mesmos). Nada aqui altera o registro nem
substitui o Explorer: tudo roda como app comum ao lado dele.

## Programas
- `bin\Arquivos.exe [pasta]`: gerenciador de arquivos no estilo do Finder (visão de ícones).
- `bin\MenuTeste.exe [pasta]`: bancada do menu de contexto (`--dump <caminho> [--bg]` grava a árvore do menu no log).

## Compilar
Precisa do Visual Studio Build Tools 2022 com C++.

```
build.cmd           (tudo)
build.cmd finder    (só Arquivos.exe)
build.cmd menu      (só MenuTeste.exe)
```

## Estrutura
- `src/menu`: menu de contexto próprio sobre as extensões reais do Windows (IContextMenu).
- `src/finder`: Arquivos (Direct2D sobre DirectComposition, acrílico do DWM).
- `src/tools`: bancadas de teste.
- `docs/problemas-conhecidos.md`: o que ainda falta ou é limitação do Windows.

## Atalhos do Arquivos
| Ação | Tecla |
|---|---|
| Abrir | duplo clique, Ctrl+O, Ctrl+↓ |
| Pasta de cima | Ctrl+↑ |
| Voltar / avançar | Alt+← / Alt+→, Ctrl+[ / Ctrl+], Backspace, botões laterais do mouse |
| Renomear | Enter ou F2 |
| Nova pasta | Ctrl+Shift+N |
| Copiar / recortar / colar | Ctrl+C / Ctrl+X / Ctrl+V |
| Mover para a Lixeira | Delete ou Ctrl+Backspace |
| Propriedades | Ctrl+I ou Alt+Enter |
| Buscar (no computador todo ou na pasta, pela barra "Buscar:") | Ctrl+F; Esc ou Voltar sai da busca |
| Nova janela | Ctrl+N |
| Nova aba / fechar aba / próxima aba | Ctrl+T / Ctrl+W / Ctrl+Tab; Ctrl+duplo clique abre a pasta em nova aba; botão do meio fecha |
| Quick Look | Espaço |
| Opções de Visualização | Ctrl+J |
| Menu com itens extras | Alt+clique direito; todas as opções: Shift+clique direito |
| Visões: ícones / lista / colunas / galeria | Ctrl+1 / Ctrl+2 / Ctrl+3 / Ctrl+4 |
| Tamanho dos ícones | Ctrl+ / Ctrl− |
| Adicionar pasta à barra lateral | Ctrl+Alt+T, ou arrastar a pasta para entre dois favoritos |
| Mover / remover favorito | arrastar na barra lateral / clique direito → Remover da Barra Lateral |
| Etiquetar | botão de etiqueta, ou arrastar os itens para uma etiqueta da barra lateral |

## Arquivos como padrão para abrir pastas (opcional, reversível)
Grava só em HKCU (verbo `arquivos` em `Directory` e `Drive`, e o comando de nova janela do Explorador, usado pelo Win+E); o valor anterior fica em `%APPDATA%\Arquivos\padrao-backup.txt`.
```
bin\Arquivos.exe --padrao status
bin\Arquivos.exe --padrao on
bin\Arquivos.exe --padrao off
```
"Este Computador" e `shell:Downloads` e afins abrem no Arquivos. Lixeira, Painel de Controle e outras pastas virtuais ainda abrem no Explorador.
