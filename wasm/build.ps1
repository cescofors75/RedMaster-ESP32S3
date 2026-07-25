param(
    [switch]$Serve,
    [int]$Port = 8080
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

function Resolve-CommandPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PrimaryPath,

        [Parameter(Mandatory = $true)]
        [string]$CommandName
    )

    if (Test-Path $PrimaryPath) {
        return $PrimaryPath
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    return $null
}

$rustupPath = Resolve-CommandPath -PrimaryPath (Join-Path $env:USERPROFILE '.cargo/bin/rustup.exe') -CommandName 'rustup.exe'
$rustcPath = Resolve-CommandPath -PrimaryPath (Join-Path $env:USERPROFILE '.cargo/bin/rustc.exe') -CommandName 'rustc.exe'
$coreTemp = 'libraydrone_core.tmp.rlib'
$wasmTemp = 'raydrone.tmp.wasm'

Remove-Item $coreTemp, $wasmTemp -Force -ErrorAction SilentlyContinue

if (-not $rustcPath) {
    throw 'rustc no encontrado. Instala Rustup primero.'
}

if ($rustupPath) {
    & $rustupPath target add wasm32-unknown-unknown 2>$null
}

try {
    & $rustcPath --edition 2021 `
        --target wasm32-unknown-unknown `
        -O -C panic=abort -C lto=fat `
        --crate-name raydrone_core --crate-type=lib `
        ..\core\src\lib.rs -o $coreTemp
    if ($LASTEXITCODE -ne 0) { throw "rustc core fallo con codigo $LASTEXITCODE" }

    & $rustcPath --edition 2021 `
        --target wasm32-unknown-unknown `
        -O -C panic=abort -C lto=fat `
        --extern raydrone_core=$coreTemp `
        --crate-type=cdylib `
        raydrone.rs -o $wasmTemp
    if ($LASTEXITCODE -ne 0) { throw "rustc wasm fallo con codigo $LASTEXITCODE" }

    Move-Item -Force $coreTemp libraydrone_core.rlib
    Move-Item -Force $wasmTemp raydrone.wasm
} finally {
    Remove-Item $coreTemp, $wasmTemp -Force -ErrorAction SilentlyContinue
}

$size = (Get-Item raydrone.wasm).Length
Write-Output "OK raydrone.wasm generado ($size bytes)"

if ($Serve) {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $python) {
        $python = Get-Command py.exe -ErrorAction SilentlyContinue
    }
    if (-not $python) {
        throw 'Python no encontrado. Instala Python o habilita el launcher py.exe.'
    }

    Write-Output "Sirviendo http://localhost:$Port/"
    & $python.Source -m http.server $Port
}