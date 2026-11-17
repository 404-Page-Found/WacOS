# Instala o WacOS neste PC (só para o seu usuário; não precisa de administrador).
#  - Programas em %LOCALAPPDATA%\Programs\WacOS (bin, icones, sons)
#  - Fontes SF Pro só para o seu usuário (se vierem na pasta "fontes")
#  - Atalho "WacOS" no Menu Iniciar e, se você quiser, início automático ao entrar no Windows
# Tudo o que muda fica anotado em %LOCALAPPDATA%\Programs\WacOS\instalacao.log
# Para desfazer: desinstalar.cmd
# -Atualizar (atualizar.cmd): só troca os programas, ícones e sons de uma instalação que já existe; não mexe em
# ajustes, fontes, cursor, atalho nem início automático, e não faz perguntas além de fechar o que está aberto.
# -Silencioso (o botão Atualizar Agora do Ajustes, tools\atualizar-github.ps1): sem perguntas; fecha o que estiver aberto
param([switch]$Atualizar, [switch]$Silencioso)
$ErrorActionPreference = 'Stop'
$src  = $PSScriptRoot
$dest = Join-Path $env:LOCALAPPDATA 'Programs\WacOS'
$log  = Join-Path $dest 'instalacao.log'
# o nome antigo (Sequoia Shell, até 16/11/2026): a instalação velha é trocada por esta
$velho = Join-Path $env:LOCALAPPDATA 'Programs\SequoiaShell'
$temVelho = Test-Path (Join-Path $velho 'bin\SequoiaShell.exe')

function Pergunta($texto) { if ($Silencioso) { return $true }; (Read-Host "$texto (s/n)").Trim().ToLower().StartsWith('s') }
function Sair { if (-not $Silencioso) { Read-Host 'Enter para sair' }; exit }
function Anota($linha) { Add-Content -Encoding utf8 $log ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $linha) }

Write-Host ("WacOS - " + $(if ($Atualizar) { "atualizar`n" } else { "instalador`n" }))
# Rodado do código-fonte (o "Code > Download ZIP" do GitHub): não tem os programas compilados. O pacote certo é o
# WacOS.zip da página de versões (17/11/2026: no outro PC deu "Start-Process ... cannot find the file specified")
if (-not (Test-Path (Join-Path $src '..\bin\WacOSShell.exe')) -and -not (Test-Path (Join-Path $src 'bin\WacOSShell.exe'))) {
    Write-Host "Esta pasta é o código-fonte do WacOS, sem os programas compilados." -ForegroundColor Yellow
    Write-Host "This folder is the WacOS source code, without the compiled programs." -ForegroundColor Yellow
    Write-Host "`nBaixe o WacOS.zip pronto em / Download the ready-made WacOS.zip from:"
    Write-Host "  https://github.com/renzyxx/WacOS/releases/latest"
    Write-Host "`nExtraia e rode o instalar.cmd de dentro dele. / Extract it and run instalar.cmd from inside it."
    Start-Process 'https://github.com/renzyxx/WacOS/releases/latest' -ErrorAction SilentlyContinue
    Sair
}
if ($Atualizar -and -not (Test-Path (Join-Path $dest 'bin\WacOSShell.exe')) -and -not $temVelho) {
    Write-Host "O WacOS ainda não está instalado neste PC ($dest). Use instalar.cmd."
    Sair
}
$shellAberto = [bool](Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue)
$build = [Environment]::OSVersion.Version.Build
if ($build -lt 22621) {
    Write-Host "Aviso: este Windows (build $build) é mais antigo que o Windows 11 22H2. Partes do Shell (vidro, captura) podem não funcionar."
    if (-not (Pergunta 'Continuar mesmo assim?')) { exit }
}

# Shell aberto (atualização): fecha do jeito certo, devolvendo a barra do Windows
Add-Type -Namespace SeqInst -Name U -MemberDefinition '[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindow(string c,string t); [DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h,uint m,System.IntPtr w,System.IntPtr l);'
if (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue) {
    if (-not $Atualizar -and -not (Pergunta 'O WacOS está aberto. Fechar para instalar a versão nova?')) { exit }
    if ($Atualizar) { Write-Host 'Fechando o WacOS para atualizar (ele abre de novo no fim)...' }
    $h = [SeqInst.U]::FindWindow('ShellMensagens', $null)
    if ($h -ne [IntPtr]::Zero) { [SeqInst.U]::PostMessage($h, 0x0312, [IntPtr]1, [IntPtr]::Zero) | Out-Null }  # o mesmo do atalho "sair do Shell"
    for ($i = 0; $i -lt 40 -and (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-Process WacOSShell, SequoiaShell -ErrorAction SilentlyContinue) { Write-Host 'O Shell não fechou. Feche-o (Ctrl+Alt+Shift+F12) e rode o instalador de novo.'; Sair }
}
# os nossos apps abertos também seguram os arquivos
$apps = 'WacOSFinder','WacOSSettings','WacOSActivityMonitor','WacOSTerminal','WacOSCalculator','WacOSNotes','WacOSScreenshot','WacOSWindow32Host',
        'SequoiaFinder','SequoiaSettings','SequoiaActivityMonitor','SequoiaTerminal','SequoiaCalculator','SequoiaNotes','SequoiaScreenshot','SequoiaWindow32Host'
$abertos = Get-Process $apps -ErrorAction SilentlyContinue
if ($abertos) {
    Write-Host ("Abertos: " + (($abertos | Select-Object -ExpandProperty ProcessName -Unique) -join ', '))
    if (-not (Pergunta 'Fechar esses apps?')) { exit }
    $abertos | Stop-Process -Force
    Start-Sleep 1
}

# 1. Programas, ícones e sons
New-Item -ItemType Directory -Force $dest | Out-Null
Anota "instalando de $src"
foreach ($pasta in 'bin', 'icones', 'sons', 'tools') {
    if (Test-Path "$src\$pasta") {
        robocopy "$src\$pasta" "$dest\$pasta" /E /NFL /NDL /NJH /NJS /NP /XF *.log | Out-Null
        Anota "copiado: $pasta"
    }
}
Get-ChildItem $dest -Recurse -File | Unblock-File  # veio da internet/zip: sem o aviso de "arquivo baixado"
Write-Host "Programas copiados para $dest"

# 1b. Instalação com o nome antigo (Sequoia Shell): a tarefa do login e o atalho passam para o WacOS e a pasta velha
#     sai. Os caminhos que o Windows guardava (Win+E, Música, Shell do Windows) o WacOSShell corrige ao abrir.
if ($temVelho) {
    $exeNovo = Join-Path $dest 'bin\WacOSShell.exe'
    $t = Get-ScheduledTask -TaskName 'SequoiaShell' -ErrorAction SilentlyContinue
    if ($t) {
        $a = New-ScheduledTaskAction -Execute $exeNovo -WorkingDirectory (Split-Path $exeNovo)
        Register-ScheduledTask -TaskName 'WacOS' -Action $a -Trigger $t.Triggers -Settings $t.Settings -Principal $t.Principal `
            -Description 'Inicia o WacOS ao entrar no Windows (desinstalar.cmd remove)' -Force | Out-Null
        Unregister-ScheduledTask -TaskName 'SequoiaShell' -Confirm:$false
        Anota 'tarefa agendada: SequoiaShell trocada por WacOS'
    }
    $lnkVelho = Join-Path ([Environment]::GetFolderPath('Programs')) 'Sequoia Shell.lnk'
    if (Test-Path $lnkVelho) {
        Remove-Item $lnkVelho
        $ws0 = New-Object -ComObject WScript.Shell
        $l0 = $ws0.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk'))
        $l0.TargetPath = $exeNovo; $l0.WorkingDirectory = Split-Path $exeNovo; $l0.IconLocation = "$exeNovo,0"
        $l0.Description = 'WacOS (barra de menus e Dock do Mac)'; $l0.Save()
        Anota 'atalho: Sequoia Shell.lnk trocado por WacOS.lnk'
    }
    # só os programas com o nome velho saem: o cursor do Mac e os ícones do sistema no registro apontam para
    # arquivos da pasta antiga (cursor\, bin\icones-sistema\), que ficam onde estão
    Get-ChildItem (Join-Path $velho 'bin') -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Sequoia*' } | Remove-Item -Force -ErrorAction SilentlyContinue
    Anota "programas antigos (Sequoia*) removidos de $velho\bin; cursor e ícones do sistema continuam lá"
    Write-Host 'A instalação antiga (Sequoia Shell) foi trocada pelo WacOS.'
}

if ($Atualizar) {  # só os arquivos; o resto (ajustes, fontes, cursor, atalho, início automático) fica como estava
    Anota 'atualização concluída'
    $exe = Join-Path $dest 'bin\WacOSShell.exe'
    if ($shellAberto) { Start-Process $exe -WorkingDirectory (Split-Path $exe); Write-Host "`nAtualizado. O WacOS abriu de novo." }
    elseif ($Silencioso) { Write-Host "`nAtualizado." }
    else { Write-Host "`nAtualizado."; if (Pergunta 'Abrir o WacOS agora?') { Start-Process $exe -WorkingDirectory (Split-Path $exe) } }
    exit
}

# 2. Fontes (só para este usuário)
$fontes = Get-ChildItem "$src\fontes" -File -Include *.otf, *.ttf -Recurse -ErrorAction SilentlyContinue
if ($fontes) {
    $fdir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'
    $freg = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
    New-Item -ItemType Directory -Force $fdir | Out-Null
    if (-not (Test-Path $freg)) { New-Item -Force $freg | Out-Null }
    $n = 0
    foreach ($f in $fontes) {
        $alvo = Join-Path $fdir $f.Name
        if (-not (Test-Path $alvo)) { Copy-Item $f.FullName $alvo; Anota "fonte: $alvo" }
        $nome = "$($f.BaseName) ($(if ($f.Extension -eq '.otf') { 'OpenType' } else { 'TrueType' }))"
        if ((Get-ItemProperty $freg -Name $nome -ErrorAction SilentlyContinue).$nome -ne $alvo) {
            Set-ItemProperty $freg -Name $nome -Value $alvo
            Anota "registro: $freg | $nome = $alvo"
        }
        $n++
    }
    Write-Host "Fontes instaladas: $n"
}

# 2b. Cursor do Mac (só para este usuário; o anterior fica anotado e o desinstalar devolve)
$cur = Get-ChildItem "$src\cursor" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cur -and (Pergunta "Usar o cursor do Mac ($($cur.Name))?")) {
    $cdir = Join-Path $dest "cursor\$($cur.Name)"
    robocopy $cur.FullName $cdir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Get-ChildItem $cdir -File | Unblock-File
    $creg = 'HKCU:\Control Panel\Cursors'
    $mapa = Get-Content "$src\cursor\cursores.txt" -Encoding utf8  # papel=arquivo (ex.: Arrow=arrowS.cur)
    $antes = Get-ItemProperty $creg
    foreach ($linha in $mapa) {
        if ($linha -notmatch '^([^=]+)=(.+)$') { continue }
        $papel = $Matches[1]; $arq = Join-Path $cdir $Matches[2]
        if (-not (Test-Path $arq)) { continue }
        Anota ("cursor-antes: {0}={1}" -f $papel, $antes.$papel)
        Set-ItemProperty $creg -Name $papel -Value $arq
    }
    Anota ("cursor-antes: (default)={0}" -f $antes.'(default)')
    Set-ItemProperty $creg -Name '(default)' -Value $cur.Name
    Set-ItemProperty $creg -Name 'Scheme Source' -Value 1 -Type DWord
    Anota "registro: $creg (cursor do Mac)"
    Add-Type -Namespace SeqCur -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, System.IntPtr c, uint d);'
    [SeqCur.U]::SystemParametersInfo(0x57, 0, [IntPtr]::Zero, 3) | Out-Null  # SPI_SETCURSORS: aplica agora
    Write-Host 'Cursor do Mac aplicado.'
}

# 3. Ajustes (se vierem no pacote e este PC ainda não tiver os seus)
$cfg = Join-Path $env:APPDATA 'Shell'
if (Test-Path "$src\ajustes") {
    New-Item -ItemType Directory -Force $cfg | Out-Null
    foreach ($f in Get-ChildItem "$src\ajustes" -File) {
        $alvo = Join-Path $cfg $f.Name
        if (-not (Test-Path $alvo)) { Copy-Item $f.FullName $alvo; Anota "ajuste: $alvo" }
    }
}

# 3b. PC novo sem ajustes: o modo do Mac completo. explorer=fundo = a área de trabalho e o botão direito são
#     os do Shell (o Explorador fica escondido, rodando só para os apps da Loja); pastas abrem no Finder.
$ajustes = Join-Path $cfg 'ajustes.txt'
if (-not (Test-Path $ajustes)) {
    New-Item -ItemType Directory -Force $cfg | Out-Null
    Set-Content -Encoding utf8 $ajustes "explorer=fundo`nfinder_padrao=1"
    Anota "ajuste: $ajustes (padrões: explorer=fundo, finder_padrao=1)"
}
# pastas, discos e Win+E abrem no Finder (só HKCU; o desinstalar devolve)
$finder = Join-Path $dest 'bin\WacOSFinder.exe'
if ((Get-Content $ajustes -ErrorAction SilentlyContinue) -notcontains 'finder_padrao=0') {
    Start-Process $finder -ArgumentList '--quieto', '--padrao', 'on' -Wait
    Anota 'pastas no Finder: WacOSFinder.exe --padrao on'
}

# 3c. Extras opcionais (cada um tem desfazer no desinstalar.cmd)
New-Item -ItemType Directory -Force (Join-Path $dest 'docs') | Out-Null   # backups dos extras ficam aqui
$tools = Join-Path $dest 'tools'
if ((Test-Path "$tools\dialogo-ligar.ps1") -and (Test-Path "$src\bin\DialogoMac.dll") -and (Pergunta 'Usar a janela de Abrir/Salvar do Mac nos programas?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\dialogo-ligar.ps1" | Out-Null
    Anota 'extra: dialogo do Mac ligado (tools\dialogo-ligar.ps1)'
    Write-Host "Janela de Abrir/Salvar do Mac ligada (os programas que a usam ficam em $cfg\dialogo.txt; '*' = todos)."
}
if ((Test-Path "$tools\icones-sistema.ps1") -and (Test-Path "$src\bin\icones-sistema") -and (Pergunta 'Trocar os ícones do próprio Windows (pastas, discos, Lixeira, tipos de arquivo) pelos do Mac?')) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\icones-sistema.ps1" | Out-Null
    Anota 'extra: icones do sistema (HKCU) aplicados (tools\icones-sistema.ps1)'
    if (Pergunta 'Também nas pastas do usuário (Documentos, Downloads...) e no ícone padrão de pasta do sistema? (pede administrador)') {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$tools\icones-sistema-extra.ps1" | Out-Null
        Anota 'extra: icones das pastas e HKLM aplicados (tools\icones-sistema-extra.ps1)'
    }
    Write-Host 'Ícones do Mac aplicados (podem levar alguns segundos para aparecer).'
}

# 4. Atalho no Menu Iniciar
$exe = Join-Path $dest 'bin\WacOSShell.exe'
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk'
$ws = New-Object -ComObject WScript.Shell
$l = $ws.CreateShortcut($lnk)
$l.TargetPath = $exe; $l.WorkingDirectory = Split-Path $exe; $l.IconLocation = "$exe,0"
$l.Description = 'WacOS (barra de menus e Dock do Mac)'
$l.Save()
Anota "atalho: $lnk"
Write-Host 'Atalho "WacOS" criado no Menu Iniciar (para fixar na barra: botão direito > Fixar na barra de tarefas)'

# 5. Iniciar com o Windows
if (Pergunta 'Abrir o WacOS sozinho toda vez que você entrar no Windows?') {
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute $exe -WorkingDirectory (Split-Path $exe)
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -Priority 4 -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName 'WacOS' -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
        -Description 'Inicia o WacOS ao entrar no Windows (desinstalar.cmd remove)' -Force | Out-Null
    Anota 'tarefa agendada: WacOS (ao entrar)'
    Write-Host 'Início automático ligado.'
}

# 6. Música como app de áudio (aparece em "Abrir com" e nos apps padrão do Windows; o padrão quem escolhe é você)
$mr = Join-Path $dest 'tools\musica-registrar.ps1'
if ((Test-Path $mr) -and (Pergunta 'Registrar o Música para abrir arquivos de áudio (você escolhe como padrão nos Ajustes do Windows)?')) {
    & $mr -Exe (Join-Path $dest 'bin\WacOSMusic.exe') -AbrirAjustes
    Anota 'Música registrado como app de áudio (HKCU\Software\Classes\SequoiaMusic.Audio; desinstalar.cmd remove)'
}

$qr = Join-Path $dest 'tools\quicktime-registrar.ps1'
if ((Test-Path $qr) -and (Pergunta 'Registrar o QuickTime Player para abrir vídeos (você escolhe como padrão nos Ajustes do Windows)?')) {
    & $qr -Exe (Join-Path $dest 'bin\WacOSQuickTime.exe') -AbrirAjustes
    Anota 'QuickTime registrado como app de vídeo (HKCU\Software\Classes\SequoiaQuickTime.Video; desinstalar.cmd remove)'
}

Anota 'instalação concluída'
Write-Host "`nPronto. Saída do Shell: Ctrl+Alt+Shift+F12. Emergência (devolve o Explorador): Ctrl+Alt+Shift+F11."
if (Pergunta 'Abrir o WacOS agora?') { Start-Process $exe -WorkingDirectory (Split-Path $exe) }
