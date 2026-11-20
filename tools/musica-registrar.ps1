# Registra o Música (WacOSMusic.exe) como app de áudio do usuário (HKCU): aparece em "Abrir com" e em
# Ajustes > Aplicativos > Aplicativos padrão, onde o usuário escolhe o Música como padrão (o Windows não deixa
# um programa trocar o padrão sozinho). -Desfazer apaga tudo o que foi criado aqui.
#   powershell -File tools\musica-registrar.ps1 [-Exe <caminho do WacOSMusic.exe>] [-Desfazer] [-AbrirAjustes]
param([string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) 'bin\WacOSMusic.exe'), [switch]$Desfazer, [switch]$AbrirAjustes)
$ErrorActionPreference = 'Stop'
$prog = 'SequoiaMusic.Audio'
$exts = '.mp3', '.m4a', '.flac', '.wav', '.aac', '.ogg', '.wma', '.aif', '.aiff', '.opus'
$cls = 'HKCU:\Software\Classes'
$capKey = 'HKCU:\Software\SequoiaShell\Musica\Capabilities'
$regName = 'Música (Sequoia)'

if ($Desfazer) {
    foreach ($e in $exts) { Remove-ItemProperty "$cls\$e\OpenWithProgids" -Name $prog -ErrorAction SilentlyContinue }
    Remove-Item "$cls\$prog" -Recurse -ErrorAction SilentlyContinue
    Remove-Item "$cls\Applications\WacOSMusic.exe" -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name $regName -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\SequoiaShell\Musica' -Recurse -ErrorAction SilentlyContinue
    "Música: registro desfeito"
} else {
    if (-not (Test-Path $Exe)) { throw "não achei $Exe" }
    $cmd = "`"$Exe`" `"%1`""
    New-Item "$cls\$prog\shell\open\command" -Force | Out-Null
    Set-ItemProperty "$cls\$prog" -Name '(default)' -Value 'Arquivo de Áudio'
    Set-ItemProperty "$cls\$prog" -Name 'FriendlyTypeName' -Value 'Arquivo de Áudio'
    New-Item "$cls\$prog\DefaultIcon" -Force | Out-Null
    Set-ItemProperty "$cls\$prog\DefaultIcon" -Name '(default)' -Value "`"$Exe`",0"
    Set-ItemProperty "$cls\$prog\shell\open\command" -Name '(default)' -Value $cmd
    New-Item "$cls\Applications\WacOSMusic.exe\shell\open\command" -Force | Out-Null
    Set-ItemProperty "$cls\Applications\WacOSMusic.exe" -Name 'FriendlyAppName' -Value 'Música'
    Set-ItemProperty "$cls\Applications\WacOSMusic.exe\shell\open\command" -Name '(default)' -Value $cmd
    New-Item "$cls\Applications\WacOSMusic.exe\SupportedTypes" -Force | Out-Null
    New-Item "$capKey\FileAssociations" -Force | Out-Null
    Set-ItemProperty $capKey -Name 'ApplicationName' -Value 'Música'
    Set-ItemProperty $capKey -Name 'ApplicationDescription' -Value 'Música do WacOS'
    Set-ItemProperty $capKey -Name 'ApplicationIcon' -Value "`"$Exe`",0"
    foreach ($e in $exts) {
        New-Item "$cls\$e\OpenWithProgids" -Force | Out-Null
        New-ItemProperty "$cls\$e\OpenWithProgids" -Name $prog -PropertyType None -Value ([byte[]]@()) -Force | Out-Null
        Set-ItemProperty "$cls\Applications\WacOSMusic.exe\SupportedTypes" -Name $e -Value ''
        Set-ItemProperty "$capKey\FileAssociations" -Name $e -Value $prog
    }
    New-Item 'HKCU:\Software\RegisteredApplications' -Force -ErrorAction SilentlyContinue | Out-Null
    Set-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name $regName -Value 'Software\SequoiaShell\Musica\Capabilities'
    "Música registrado ($Exe)"
}
# avisa o Explorador que as associações mudaram
# avisar o Explorador: pelo WacOSShell.exe (modo ajudante), sem C# embutido no script
$ajudante = Join-Path $PSScriptRoot '..\bin\WacOSShell.exe'
function AvisarIcones { if (Test-Path $ajudante) { Start-Process $ajudante -ArgumentList '--avisar-icones' -Wait } }
AvisarIcones
if ($AbrirAjustes -and -not $Desfazer) { Start-Process ("ms-settings:defaultapps?registeredAppUser=" + [uri]::EscapeDataString($regName)) }
