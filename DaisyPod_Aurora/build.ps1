param(
    [switch]$Clean,
    [switch]$VerboseBuild,
    [switch]$Direct,
    [switch]$Diagnostic
)

$ErrorActionPreference = 'Stop'
$builder = Join-Path $PSScriptRoot '..\RedMaster_DaisySeed64MB\build_daisy.ps1'

if(-not (Test-Path $builder)) {
    throw "No se encontro el helper de compilacion Daisy: $builder"
}

$env:DIAGNOSTIC = if($Diagnostic) { '1' } else { '0' }

if($Direct) {
    # BOOT_NONE genera una imagen para la flash interna (0x08000000). Aurora
    # cabe holgadamente y este perfil funciona incluso sin bootloader Daisy.
    $env:APP_TYPE = 'BOOT_NONE'
    $selectedBuildDir = if($Diagnostic) { 'build_diag_direct' } else { 'build_direct' }
    & $builder `
        -ProjectDir $PSScriptRoot `
        -BuildDir $selectedBuildDir `
        -Clean:$Clean `
        -VerboseBuild:$VerboseBuild
} else {
    $env:APP_TYPE = 'BOOT_SRAM'
    $selectedBuildDir = if($Diagnostic) { 'build_diag' } else { 'build' }
    & $builder `
        -ProjectDir $PSScriptRoot `
        -BuildDir $selectedBuildDir `
        -Clean:$Clean `
        -VerboseBuild:$VerboseBuild
}

exit $LASTEXITCODE
