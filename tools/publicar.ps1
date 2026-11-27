# Publishes a new WacOS version on GitHub with the FULL package (programs, icons, SF fonts, macOS sounds and
# cursor), built on this PC: the fonts, sounds and cursor are not in the repository, so GitHub Actions can't build
# this package (it only checks that the code compiles).
#   1. next version = build-<highest build-N + 1>
#   2. writes bin\versao.txt (version, commit, date): the updater compares it with the latest release
#   3. tools\empacotar.ps1 -SemSistema -SemAjustes (no personal settings) -> dist\WacOS-<date>.zip
#   4. git push, then a release with that zip as the latest
# Run it from a normal PowerShell of the user (it reads the installed fonts and the cursor in use).
#   BETA-TEST: com a edição em Rust gerada (rust-teste\build-rust.ps1 -Zip), o WacOSTroca.exe vai no bin\ do pacote
#   e o WacOS-Rust.zip sobe como asset da mesma release (o WacOSTroca --instalar baixa dela). -RustDist aponta a pasta
#   dist da edição Rust (padrão: a do worktree do port).
param([string]$Notas = '', [string]$RustDist = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$repo = 'renzyxx/WacOS'
$log = Join-Path $root 'dist\publicar.log'
function L($s) { $s | Tee-Object -FilePath $log -Append | Write-Host }
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
"" | Set-Content $log

# o maior build-N + 1 (não a contagem: as builds 1 a 16 foram apagadas em 20/11/2026, na troca de licença)
$n = (@(gh release list -R $repo --limit 1000 --json tagName --jq '.[].tagName' | Where-Object { $_ -match '^build-(\d+)$' } | ForEach-Object { [int]($_ -replace '^build-', '') }) + @(17) | Measure-Object -Maximum).Maximum + 1
$tag = "build-$n"
git push -q origin main 2>&1 | Out-Null
$sha = (git rev-parse HEAD).Trim()
L "versão: $tag ($sha)"
Set-Content -Encoding utf8 (Join-Path $root 'bin\versao.txt') @($tag, $sha, (Get-Date -Format 'yyyy-MM-dd HH:mm'))

# BETA-TEST: o trocador entra no pacote do C++ e o zip da edição Rust vai junto na release
if (-not $RustDist) {
    foreach ($c in @((Join-Path $root 'rust-teste\dist'), (Join-Path $root '.claude\worktrees\rust-port-teste-23da8b\rust-teste\dist'))) {
        if (Test-Path (Join-Path $c 'WacOS-Rust.zip')) { $RustDist = $c; break }
    }
}
$rustZip = $null
if ($RustDist -and (Test-Path (Join-Path $RustDist 'WacOS-Rust.zip'))) {
    $rustZip = Get-Item (Join-Path $RustDist 'WacOS-Rust.zip')
    $troca = Join-Path $RustDist 'WacOS-Rust\bin\WacOSTroca.exe'
    if (Test-Path $troca) { Copy-Item $troca (Join-Path $root 'bin\WacOSTroca.exe') -Force; L "beta: WacOSTroca.exe copiado para bin" }
    else { L 'AVISO: WacOSTroca.exe não encontrado; o BETA-TEST não vai funcionar nesta build' }
} else { L 'AVISO: WacOS-Rust.zip não encontrado; a release sai sem a edição em Rust (BETA-TEST)' }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'empacotar.ps1') -SemSistema -SemAjustes *>&1 | ForEach-Object { L "$_" }
if ($LASTEXITCODE -ne 0) { L 'ERRO: o empacotar.ps1 falhou'; exit 1 }  # senão subia o zip mais recente, de outra rodada
$zip = Get-ChildItem (Join-Path $root 'dist') -Filter 'WacOS-*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $zip) { L 'ERRO: o pacote não foi gerado'; exit 1 }

if (-not $Notas) {
    $antes = gh release list -R $repo --limit 1 --json tagName --jq '.[0].tagName'
    $Notas = if ($antes) { (git log --pretty=format:'- %s' "$antes..HEAD") -join "`n" } else { '- New version' }
}
$nf = Join-Path $root 'dist\notas.md'
Set-Content -Encoding utf8 $nf $Notas
$assets = @($zip.FullName)
if ($rustZip) { $assets += $rustZip.FullName }  # o WacOSTroca --instalar procura WacOS-Rust*.zip na release mais nova
gh release create $tag @assets -R $repo --target $sha --title "WacOS $tag" --notes-file $nf --latest 2>&1 | ForEach-Object { L "$_" }
L "publicado: $tag ($([math]::Round($zip.Length / 1MB)) MB)"
