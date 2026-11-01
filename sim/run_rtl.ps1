param([Parameter(Mandatory)][string]$VivadoRoot)
$ErrorActionPreference = 'Stop'
$src = Join-Path (Split-Path $PSScriptRoot -Parent) 'src'
& (Join-Path $src 'scripts/test_shared_store.ps1') -VivadoRoot $VivadoRoot
