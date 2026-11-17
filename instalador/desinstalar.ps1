# Remove o WacOS instalado pelo instalar.ps1: fecha o Shell (devolvendo a barra do Windows), tira o
# início automático, o atalho, as fontes que o instalador colocou e a pasta dos programas.
# Seus ajustes (%APPDATA%\Shell) só saem se você disser que sim.
$ErrorActionPreference = 'Continue'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\WacOS'
$log  = Join-Path $dest 'instalacao.log'
function Pergunta($texto) { (Read-Host "$texto (s/n)").Trim().ToLower().StartsWith('s') }

Write-Host "WacOS - desinstalar`n"
if (-not (Pergunta "Remover o WacOS de $dest?")) { exit }

Add-Type -Namespace SeqDes -Name U -MemberDefinition '[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindow(string c,string t); [DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h,uint m,System.IntPtr w,System.IntPtr l);'
if (Get-Process WacOSShell -ErrorAction SilentlyContinue) {
    $h = [SeqDes.U]::FindWindow('ShellMensagens', $null)
    if ($h -ne [IntPtr]::Zero) { [SeqDes.U]::PostMessage($h, 0x0312, [IntPtr]1, [IntPtr]::Zero) | Out-Null }
    for ($i = 0; $i -lt 40 -and (Get-Process WacOSShell -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
}
if (Get-Process WacOSShell -ErrorAction SilentlyContinue) { Write-Host 'O Shell não fechou: feche com Ctrl+Alt+Shift+F12 e rode de novo.'; Read-Host 'Enter para sair'; exit }

# Janelas dos apps com a barra do Mac: o Shell manda cada uma voltar ao normal ao sair, mas com pouco tempo
# (300 ms) e uma vez só; app ocupado ou janela escondida ficava com a barra do Mac. Aqui manda de novo, com
# mais tempo, a todas que ainda estão marcadas, e confere.
Add-Type -Namespace SeqDes -Name J -MemberDefinition @'
public delegate bool EP(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EP f, System.IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern System.IntPtr GetPropW(System.IntPtr h, string s);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string s);
[DllImport("user32.dll")] public static extern System.IntPtr SendMessageTimeoutW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l, uint f, uint t, out System.IntPtr r);
public static int Undo(bool send) {
    uint msg = RegisterWindowMessageW("MacJanelaDesfazer"); int n = 0;
    EnumWindows((h, l) => {
        if (GetPropW(h, "MacJanela") != System.IntPtr.Zero || GetPropW(h, "MacJanelaLeve") != System.IntPtr.Zero) {
            n++; System.IntPtr r; if (send) SendMessageTimeoutW(h, msg, System.IntPtr.Zero, System.IntPtr.Zero, 2 /*SMTO_ABORTIFHUNG*/, 3000, out r);
        }
        return true; }, System.IntPtr.Zero);
    return n;
}
'@
$marcadas = [SeqDes.J]::Undo($true)
Start-Sleep -Milliseconds 500
$sobrou = [SeqDes.J]::Undo($false)
if ($marcadas) { Write-Host ("Janelas devolvidas ao visual do Windows: {0}" -f ($marcadas - $sobrou)) }
if ($sobrou) { Write-Host "$sobrou janela(s) não responderam (app travado): voltam ao normal quando o app for fechado e aberto de novo." }
Get-Process WacOSFinder, WacOSSettings, WacOSActivityMonitor, WacOSTerminal, WacOSCalculator, WacOSNotes, WacOSScreenshot, WacOSWindow32Host -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }

# barra do Windows nos outros monitores (o Shell desliga enquanto roda e devolve ao sair; se não devolveu, devolve aqui)
$cfgAj = Join-Path $env:APPDATA 'Shell\ajustes.txt'
$antes = if (Test-Path $cfgAj) { (Get-Content $cfgAj -Encoding utf8 | Where-Object { $_ -match '^barra_windows_monitores_antes=(.+)$' } | Select-Object -First 1) } else { $null }
if ($antes -match '=(.+)$') {
    $v = $Matches[1].Trim(); $adv = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
    if ($v -eq 'x') { Remove-ItemProperty $adv -Name MMTaskbarEnabled -ErrorAction SilentlyContinue } else { Set-ItemProperty $adv -Name MMTaskbarEnabled -Value ([int]$v) -Type DWord }
    (Get-Content $cfgAj -Encoding utf8) | Where-Object { $_ -notmatch '^barra_windows_monitores_antes=' } | Set-Content $cfgAj -Encoding utf8
    Write-Host 'Barra do Windows nos outros monitores devolvida.'
}

Unregister-ScheduledTask -TaskName 'WacOS' -Confirm:$false -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'SequoiaShell' -Confirm:$false -ErrorAction SilentlyContinue  # nome antigo
# pastas voltam a abrir no Explorador (desfaz o --padrao on)
$finder = Join-Path $dest 'bin\WacOSFinder.exe'
if (Test-Path $finder) { Start-Process $finder -ArgumentList '--quieto', '--padrao', 'off' -Wait }
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'WacOS.lnk') -ErrorAction SilentlyContinue

# extras: janela de Abrir/Salvar e ícones do sistema voltam aos do Windows. Decide pelo que está aplicado de
# verdade (chaves e backups), não só pelo log: instalação antiga, atualizada ou com o log perdido também volta.
$dlg = $false
foreach ($raiz in 'HKCU:\Software\Classes\CLSID', 'HKCU:\Software\Classes\WOW6432Node\CLSID') {
    foreach ($clsid in '{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}', '{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}') {
        $k = "$raiz\$clsid"
        if ((Test-Path $k) -and ((Get-ItemProperty "$k\InprocServer32" -ErrorAction SilentlyContinue).'(default)' -match 'DialogoMac')) { Remove-Item -Recurse -Force $k; $dlg = $true }
    }
}
if ($dlg) { Write-Host 'Janela de Abrir/Salvar do Windows devolvida.' }
$docs = Join-Path $dest 'docs'
if ((Test-Path "$docs\backup-icones-sistema.json") -or (Test-Path "$docs\backup-desktopini") -or (Test-Path "$docs\backup-icones-maquina.json")) {
    if (Test-Path "$dest\tools\icones-sistema.ps1") {
        Write-Host 'Devolvendo os ícones do Windows (se pedir administrador, aceite: é para desfazer a parte das pastas)...'
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$dest\tools\icones-sistema.ps1" -Desfazer
        Get-Content "$dest\bin\icones-sistema\ultima-execucao*.txt" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }
        if ((Test-Path "$docs\backup-icones-sistema.json") -or (Test-Path "$docs\backup-desktopini") -or (Test-Path "$docs\backup-icones-maquina.json")) {
            Write-Host 'Parte dos ícones não voltou. A pasta da instalação fica (os backups estão nela); rode o desinstalar de novo.'
            Read-Host 'Enter para sair'; exit
        }
        Write-Host 'Ícones do Windows devolvidos.'
    } else { Write-Host "Os ícones do Mac estão aplicados, mas falta $dest\tools\icones-sistema.ps1 para desfazer. Nada foi apagado."; Read-Host 'Enter para sair'; exit }
}

# cursor: volta o que estava antes
if (Test-Path $log) {
    $creg = 'HKCU:\Control Panel\Cursors'
    $voltou = $false
    foreach ($l in Get-Content $log) {
        if ($l -match 'cursor-antes: ([^=]+)=(.*)$') { Set-ItemProperty $creg -Name $Matches[1] -Value $Matches[2]; $voltou = $true }
    }
    if ($voltou) {
        Add-Type -Namespace SeqCurD -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, System.IntPtr c, uint d);'
        [SeqCurD.U]::SystemParametersInfo(0x57, 0, [IntPtr]::Zero, 3) | Out-Null
    }
}

# fontes e entradas de registro que o instalador anotou
if (Test-Path $log) {
    foreach ($l in Get-Content $log) {
        if ($l -match 'registro: (.+?) \| (.+?) = (.+)$') { Remove-ItemProperty -Path $Matches[1] -Name $Matches[2] -ErrorAction SilentlyContinue }
        elseif ($l -match 'fonte: (.+)$') { Remove-Item $Matches[1] -ErrorAction SilentlyContinue }
    }
}
Remove-Item 'HKCU:\Software\ShellProprio' -Recurse -Force -ErrorAction SilentlyContinue  # cores das barras e opções da DLL das janelas
# QuickTime registrado como app de vídeo (tools\quicktime-registrar.ps1)
$qr = Join-Path $dest 'tools\quicktime-registrar.ps1'; if (Test-Path $qr) { & $qr -Desfazer }
# Música registrado como app de áudio (tools\musica-registrar.ps1): sai de "Abrir com" e dos apps padrão
$mr = Join-Path $dest 'tools\musica-registrar.ps1'
if (Test-Path $mr) { & $mr -Desfazer } else {
    foreach ($e in '.mp3', '.m4a', '.flac', '.wav', '.aac', '.ogg', '.wma', '.aif', '.aiff', '.opus') { Remove-ItemProperty "HKCU:\Software\Classes\$e\OpenWithProgids" -Name 'SequoiaMusic.Audio' -ErrorAction SilentlyContinue }
    Remove-Item 'HKCU:\Software\Classes\SequoiaMusic.Audio', 'HKCU:\Software\Classes\Applications\WacOSMusic.exe', 'HKCU:\Software\SequoiaShell\Musica' -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name 'Música (Sequoia)' -ErrorAction SilentlyContinue
}
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path $dest) { Write-Host "Alguns arquivos estavam em uso; apague $dest depois de reiniciar o PC." }

$cfg = Join-Path $env:APPDATA 'Shell'
if ((Test-Path $cfg) -and (Pergunta "Apagar também os seus ajustes ($cfg)?")) { Remove-Item $cfg -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'WacOS removido.'
Read-Host 'Enter para fechar'
