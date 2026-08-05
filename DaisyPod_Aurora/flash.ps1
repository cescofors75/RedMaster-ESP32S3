param(
    [string]$DfuUtil,
    [int]$TimeoutSeconds = 45,
    [switch]$Direct,
    [switch]$Diagnostic
)

$ErrorActionPreference = 'Stop'

if($Direct) {
    $directFolder  = if($Diagnostic) { 'build_diag_direct' } else { 'build_direct' }
    $firmware      = Join-Path $PSScriptRoot "$directFolder\Aurora.bin"
    $flashAddress  = '0x08000000:leave'
    $expectedDfu   = 'Internal Flash|08000000'
    $profileName   = 'DFU interno STM32 / BOOT_NONE'
    $buttonHelp    = @(
        'Usa los dos botones diminutos de la placa Daisy Seed, no Button 1/2 del panel:',
        '1) manten pulsado BOOT;',
        '2) pulsa y suelta RESET;',
        '3) suelta BOOT.',
        'Aviso: este perfil ocupa la flash interna y reemplaza cualquier bootloader Daisy instalado.'
    ) -join [Environment]::NewLine
} else {
    $bootFolder    = if($Diagnostic) { 'build_diag' } else { 'build' }
    $firmware      = Join-Path $PSScriptRoot "$bootFolder\Aurora.bin"
    $flashAddress  = '0x90040000:leave'
    $expectedDfu   = 'QSPI|90000000'
    $profileName   = 'bootloader Daisy / BOOT_SRAM'
    $buttonHelp    = @(
        'Pulsa RESET una vez sin mantener BOOT.',
        'Mientras el LED de la Daisy Seed respira, pulsa BOOT para mantener abierta la ventana.'
    ) -join [Environment]::NewLine
}

if(-not (Test-Path $firmware)) {
    $diagArg = if($Diagnostic) { ' -Diagnostic' } else { '' }
    $buildCommand = if($Direct) { ".\build.ps1 -Direct$diagArg -Clean" } else { ".\build.ps1$diagArg -Clean" }
    throw "No existe $firmware. Ejecuta primero: $buildCommand"
}

if(-not $DfuUtil) {
    $fromPath = Get-Command dfu-util.exe -ErrorAction SilentlyContinue
    if($fromPath) {
        $DfuUtil = $fromPath.Source
    } else {
        $known = Get-ChildItem 'C:\Espressif\tools\dfu-util' `
            -Filter 'dfu-util.exe' -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if($known) { $DfuUtil = $known.FullName }
    }
}

if(-not $DfuUtil -or -not (Test-Path $DfuUtil)) {
    throw 'No se encontro dfu-util.exe. Pasa su ruta con -DfuUtil.'
}

Write-Host "Perfil: $profileName" -ForegroundColor Cyan
Write-Host $buttonHelp -ForegroundColor Yellow
Write-Host "Esperando el dispositivo correcto durante $TimeoutSeconds s..." -ForegroundColor Cyan

$deadline      = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$ready         = $false
$sawWrongDfu   = $false
$sawDriverError = $false
$lastListing   = ''
$nextProgress  = [DateTime]::UtcNow.AddSeconds(5)

while([DateTime]::UtcNow -lt $deadline) {
    $listing = & $DfuUtil -l 2>&1 | Out-String
    if($listing -match '0483:df11') {
        $lastListing = $listing
        if($listing -match 'Cannot open|LIBUSB_ERROR_ACCESS|Access denied') {
            $sawDriverError = $true
        }
        if($listing -match $expectedDfu) {
            $ready = $true
            break
        }
        $sawWrongDfu = $true
    }

    if([DateTime]::UtcNow -ge $nextProgress) {
        $remaining = [math]::Max(0, [math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
        Write-Host "Esperando USB DFU... $remaining s" -ForegroundColor DarkGray
        $nextProgress = [DateTime]::UtcNow.AddSeconds(5)
    }
    Start-Sleep -Milliseconds 250
}

if(-not $ready) {
    if($sawDriverError) {
        Write-Host $lastListing -ForegroundColor DarkYellow
        throw 'Windows ve STM32 DFU, pero dfu-util no puede abrirlo. Asigna el driver WinUSB al dispositivo STM32 BOOTLOADER usando Zadig y repite.'
    }
    if($sawWrongDfu) {
        $wanted = if($Direct) { 'DFU interno con @Internal Flash' } else { 'bootloader Daisy con @QSPI' }
        Write-Host $lastListing -ForegroundColor DarkYellow
        if(-not $Direct -and $lastListing -match 'Internal Flash|08000000') {
            throw 'La Daisy esta correctamente en DFU interno. Ejecutaste el perfil QSPI. Usa exactamente: .\flash.ps1 -Direct'
        }
        if($Direct -and $lastListing -match 'QSPI|90000000') {
            throw 'La Daisy esta en el bootloader Daisy/QSPI. Ejecuta el perfil normal: .\flash.ps1'
        }
        throw "Se detecto un DFU 0483:df11, pero no es el modo requerido: $wanted. Repite la secuencia indicada arriba."
    }

    $driverHint = @(
        'Windows no detecto ningun dispositivo USB 0483:df11.',
        'Comprueba cable USB de datos, otro puerto USB y usa BOOT/RESET de la Daisy Seed (no los botones musicales del panel).',
        'Si aparece STM32 BOOTLOADER con advertencia en Administrador de dispositivos, instala/asigna el driver WinUSB con Zadig.'
    ) -join [Environment]::NewLine
    throw $driverHint
}

Write-Host "DFU correcto detectado. Escribiendo $flashAddress..." -ForegroundColor Green
$flashOut = & $DfuUtil -a 0 -s $flashAddress -D $firmware -d ',0483:df11' 2>&1 | Out-String
$flashRc  = $LASTEXITCODE

if($flashOut) { Write-Host $flashOut }
if($flashRc -ne 0) {
    $payloadComplete = $flashOut -match 'Download done\.' `
                       -and $flashOut -match 'File downloaded successfully'
    $leftDfuEarly = $flashOut -match 'Error during download get_status'

    # En STM32 ROM DFU sobre Windows, :leave puede reiniciar y retirar el USB
    # antes de que dfu-util lea el ultimo estado. El proceso devuelve 74 aunque
    # toda la imagen haya sido aceptada. Solo se admite como exito si aparecen
    # ambas confirmaciones de descarga completa y el unico fallo es get_status.
    if($Direct -and $flashRc -eq 74 -and $payloadComplete -and $leftDfuEarly) {
        Write-Host 'La descarga termino correctamente; la Daisy se reinicio antes del ultimo get_status (codigo 74 no fatal).' -ForegroundColor Yellow
    } else {
        throw "dfu-util termino con codigo $flashRc. No se considera cargado el firmware."
    }
}

$loadedName = if($Diagnostic) { 'Diagnostico passthrough' } else { 'Aurora' }
Write-Host "$loadedName cargado correctamente." -ForegroundColor Green
