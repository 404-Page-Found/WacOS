﻿# Atualiza o WacOS pelo GitHub (Ajustes > Geral > Atualização de Software).
# Cada commit no GitHub gera um pacote novo (GitHub Actions, .github\workflows\build.yml) publicado como a versão
# "latest", com bin\versao.txt dentro (1ª linha: a versão, ex. build-12; 2ª: o commit; 3ª: a data).
#  -Verificar -Saida <arquivo>: só confere (o Ajustes chama ao abrir a página) e grava no arquivo, uma por linha:
#     atual=<versão instalada>  novo=<versão mais nova>  data=<data dela>  erro=<motivo>  commit=<mensagem> (o que falta)
#  sem -Verificar: baixa o .zip, extrai numa pasta temporária e roda o instalar.ps1 -Atualizar -Silencioso de dentro
#     dele (troca só programas, ícones, sons e ferramentas; os ajustes ficam)
# Repositório privado: o token do GitHub vai em %APPDATA%\Shell\github-token.txt (só leitura basta).
param([switch]$Verificar, [string]$Saida = '', [string]$Bin = '')
$ErrorActionPreference = 'Stop'
$repo = 'renzyxx/WacOS'
if (-not $Bin) { $Bin = Join-Path $env:LOCALAPPDATA 'Programs\WacOS\bin' }
$out = New-Object System.Collections.Generic.List[string]

function Grava { if ($Saida) { Set-Content -Encoding utf8 $Saida $out } }
function Fim($msg, $espera = 4) { if ($Verificar) { Grava; exit }; Write-Host $msg; Start-Sleep $espera; exit }
function Erro($msg) { $out.Add("erro=$msg"); Fim $msg 8 }

if (-not $Verificar) { $host.UI.RawUI.WindowTitle = 'WacOS - Atualização' }
$atual = ''; $atualSha = ''
$vf = Join-Path $Bin 'versao.txt'
if (Test-Path $vf) { $v = @(Get-Content $vf); if ($v.Count -ge 1) { $atual = $v[0].Trim() }; if ($v.Count -ge 2) { $atualSha = $v[1].Trim() } }
$out.Add("atual=$atual")
if (-not $Verificar) { Write-Host "WacOS instalado: $(if ($atual) { $atual } else { 'versão desconhecida' })" }

$h = @{ 'User-Agent' = 'WacOS-atualizador'; 'Accept' = 'application/vnd.github+json' }
$tf = Join-Path $env:APPDATA 'Shell\github-token.txt'
if (Test-Path $tf) { $h['Authorization'] = 'Bearer ' + (Get-Content $tf -TotalCount 1).Trim() }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Verificar) { Write-Host 'Procurando a versão mais nova no GitHub...' }
try { $rel = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo/releases/latest" }
catch {
    $code = 0; try { $code = $_.Exception.Response.StatusCode.value__ } catch {}
    if ($code -eq 404) { Erro "nenhuma versão publicada em github.com/$repo (repositório privado? token em $tf)" }
    Erro "sem conexão com o GitHub ($($_.Exception.Message))"
}
$novo = $rel.tag_name
$out.Add("novo=$novo")
$out.Add("data=$(([datetime]$rel.published_at).ToLocalTime().ToString('dd/MM/yyyy HH:mm'))")
if ($atual -and $atual -eq $novo) { Fim 'O WacOS já está atualizado.' }

# o que falta: os commits entre a versão instalada e a nova (sem o commit instalado, os do texto da versão)
$msgs = @()
if ($atualSha) {
    try {
        $cmp = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo/compare/$atualSha...$novo"
        $msgs = @($cmp.commits | ForEach-Object { ($_.commit.message -split "`n")[0] })
        [array]::Reverse($msgs)  # o mais novo primeiro
    } catch {}
}
if (-not $msgs -and $rel.body) { $msgs = @($rel.body -split "`n" | ForEach-Object { $_.Trim().TrimStart('-', '*', ' ') } | Where-Object { $_ }) }
foreach ($m in ($msgs | Select-Object -First 40)) { $out.Add("commit=$m") }
if ($Verificar) { Fim '' }

Write-Host "Versão mais nova: $novo"
if ($msgs) { Write-Host 'O que muda:'; $msgs | Select-Object -First 20 | ForEach-Object { Write-Host "  - $_" } }
$asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
if (-not $asset) { Fim 'A versão mais nova não tem o pacote .zip.' 8 }
$tmp = Join-Path $env:TEMP ('wacos-atualizar-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp $asset.name
Write-Host "Baixando $($asset.name) ($([math]::Round($asset.size / 1MB)) MB)..."
$dh = $h.Clone(); $dh['Accept'] = 'application/octet-stream'
$ProgressPreference = 'SilentlyContinue'  # a barra de progresso do PowerShell deixa o download muito lento
Invoke-WebRequest -UseBasicParsing -Headers $dh -Uri $asset.url -OutFile $zip
Write-Host 'Extraindo...'
Expand-Archive $zip (Join-Path $tmp 'pacote') -Force
$script = Get-ChildItem (Join-Path $tmp 'pacote') -Recurse -Filter instalar.ps1 | Select-Object -First 1
if (-not $script) { Fim 'O pacote não tem o instalar.ps1.' 8 }
Write-Host 'Instalando (só os arquivos que mudaram; o WacOS fecha e abre de novo sozinho)...'
& powershell -NoProfile -ExecutionPolicy Bypass -File $script.FullName -Atualizar -Silencioso
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Fim "Pronto: WacOS $novo instalado."
