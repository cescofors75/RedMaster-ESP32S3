param()

# ══════════════════════════════════════════════════════════════════
#  Flash DEMO BELLS — firmware standalone (síntesis pura, sin samples)
#  Compila con: build_daisy.ps1 -DemoBells  (o make DEMO_BELLS=1)
#  Flashea build/DemoBells.bin
#  vía DFU del bootloader Daisy (QSPI @ 0x90040000).
# ══════════════════════════════════════════════════════════════════

$ErrorActionPreference = 'Continue'
$repoRoot = $PSScriptRoot
$root   = Join-Path $repoRoot 'DaisySeed'
$dfu    = 'C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64\dfu-util.exe'
$boot   = Join-Path $root 'libdaisy\core\dsy_bootloader_v6_4-intdfu-2000ms.bin'
$fw     = Join-Path $root 'build\DemoBells.bin'
$gccBin = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools\bin'
$makeBin= 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845\tools\bin'
$log    = Join-Path $repoRoot 'flash_bells_script_log.txt'

$env:PATH = "$gccBin;$makeBin;$env:PATH"
Set-Location $root

"=== FLASH DEMO BELLS $(Get-Date -Format s) ===" | Out-File -FilePath $log -Encoding utf8

if(-not (Test-Path $fw)) {
    Write-Host "No existe $fw — compila primero: build_daisy.ps1 -DemoBells" -ForegroundColor Red
    'RESULT=NO_FIRMWARE' | Tee-Object -FilePath $log -Append
    exit 1
}

Write-Host 'PASO 1: Presiona BOOT + RESET en la Daisy (ROM DFU)...' -ForegroundColor Yellow

$found = $false
$bootloaderDfu = $false
for($i = 0; $i -lt 240; $i++) {
    $list = & $dfu -l 2>&1 | Out-String
    if($list -match 'df11') {
        $found = $true
        if($list -match '@Flash|name="Flash|name="QSPI') { $bootloaderDfu = $true }
        "DFU detectado en t=$($i/2)s" | Tee-Object -FilePath $log -Append
        break
    }
    if($i % 20 -eq 0) { Write-Host "[$($i/2)s] esperando DFU..." -ForegroundColor DarkGray }
    Start-Sleep -Milliseconds 500
}

if(-not $found) {
    'RESULT=TIMEOUT_DFU' | Tee-Object -FilePath $log -Append
    Write-Host 'TIMEOUT: no se detecto DFU' -ForegroundColor Red
    exit 1
}

if($bootloaderDfu) {
    Write-Host 'DFU del bootloader Daisy ya activo: salto el bootloader interno' -ForegroundColor Green
} else {
    Write-Host 'Flasheando bootloader interno...' -ForegroundColor Cyan
    $resBoot = & $dfu -a 0 -s 0x08000000:leave -D $boot -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String
    if($resBoot -notmatch 'Download done') {
        'RESULT=BOOT_FLASH_FAIL' | Tee-Object -FilePath $log -Append
        Write-Host 'Fallo al flashear bootloader interno' -ForegroundColor Red
        exit 2
    }

    Write-Host 'PASO 2: Desconecta y reconecta USB SIN botones (bootloader DFU)...' -ForegroundColor Yellow
    $found2 = $false
    for($i = 0; $i -lt 600; $i++) {
        $list = & $dfu -l 2>&1 | Out-String
        if($list -match 'df11') { $found2 = $true; break }
        if($i % 20 -eq 0) { Write-Host "[$($i/2)s] esperando reconexion..." -ForegroundColor DarkGray }
        Start-Sleep -Milliseconds 500
    }
    if(-not $found2) {
        'RESULT=TIMEOUT_DFU_BOOTLOADER' | Tee-Object -FilePath $log -Append
        Write-Host 'TIMEOUT: no aparecio DFU del bootloader' -ForegroundColor Red
        exit 3
    }
}

Write-Host 'Flasheando DemoBells (QSPI @ 0x90040000)...' -ForegroundColor Cyan
$resApp = & $dfu -a 0 -s 0x90040000:leave -D $fw -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String

if($resApp -notmatch 'Download done') {
    'RESULT=FLASH_FAIL' | Tee-Object -FilePath $log -Append
    Write-Host 'FLASH_FAIL (DemoBells)' -ForegroundColor Red
    exit 4
}

'RESULT=FLASH_OK' | Tee-Object -FilePath $log -Append
Write-Host 'FLASH_OK — la demo debe sonar al arrancar' -ForegroundColor Green
exit 0
