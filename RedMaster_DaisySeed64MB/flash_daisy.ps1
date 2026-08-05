param(
    [switch]$SkipSamples,
    [switch]$DemoBells,
    [switch]$ShowcaseDemo
)

$ErrorActionPreference = 'Continue'
$repoRoot = $PSScriptRoot
$root   = Join-Path $repoRoot 'DaisySeed'
$dfu    = 'C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64\dfu-util.exe'
$bootPrimary  = Join-Path $root 'libdaisy\core\dsy_bootloader_v6_4-intdfu-2000ms.bin'
$bootFallback = Join-Path $root 'libdaisy\core\dsy_bootloader_v6_4-extdfu-2000ms.bin'

# ── Selección de firmware ──
# -DemoBells   → build/DemoBells.bin (síntesis pura, sin samples)
# -ShowcaseDemo → build_showcase/DrumMachine.bin (show musical autónomo)
# por defecto → build/DrumMachine.bin (+ samples.bin)
if($DemoBells) {
    $fw          = Join-Path $root 'build\DemoBells.bin'
    $SkipSamples = $true   # la demo no usa samples
} elseif($ShowcaseDemo) {
    $fw          = Join-Path $root 'build_showcase\DrumMachine.bin'
    $SkipSamples = $true   # showcase usa los motores generados
} else {
    $fw          = Join-Path $root 'build\DrumMachine.bin'
}
$wavblob= Join-Path $root 'build\samples.bin'
$gccBin = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools\bin'
$makeBin= 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.2.0.202409170845\tools\bin'
$log    = Join-Path $repoRoot 'flash_daisy_script_log.txt'

$env:PATH = "$gccBin;$makeBin;$env:PATH"
Set-Location $root

function Invoke-PythonScript {
    param(
        [string]$ScriptPath
    )

    $launchers = @(
        @('py', '-3', $ScriptPath),
        @('python', $ScriptPath)
    )

    foreach($launcher in $launchers) {
        $cmd = $launcher[0]
        if(Get-Command $cmd -ErrorAction SilentlyContinue) {
            & $cmd $launcher[1..($launcher.Length - 1)]
            return $LASTEXITCODE
        }
    }

    Write-Host 'No se encontro py/python para generar samples.bin' -ForegroundColor Yellow
    return 9009
}

function Invoke-DfuDownloadWithTimeout {
    param(
        [Parameter(Mandatory = $true)][string]$Address,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [int]$TimeoutSeconds = 30,
        [switch]$PromptReset
    )

    # dfu-util -w has no built-in timeout. Run it under supervision so a missed
    # 2-second bootloader window cannot leave the VS Code task blocked forever.
    $token  = [Guid]::NewGuid().ToString('N')
    $outTmp = Join-Path $env:TEMP "red808-dfu-$token.out"
    $errTmp = Join-Path $env:TEMP "red808-dfu-$token.err"
    $dfuArgs = @('-w', '-a', '0', '-s', $Address, '-D', $FilePath, '-d', ',0483:df11')

    try {
        $process = Start-Process -FilePath $dfu -ArgumentList $dfuArgs -PassThru `
            -RedirectStandardOutput $outTmp -RedirectStandardError $errTmp -WindowStyle Hidden

        if($PromptReset) {
            Write-Host ''
            Write-Host '>>> PULSA RESET AHORA UNA VEZ (SIN BOOT) <<<' -ForegroundColor Yellow
            Write-Host "dfu-util esta preparado; esperando la Daisy durante $TimeoutSeconds s..." -ForegroundColor Cyan
        }

        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $nextReminder = [DateTime]::UtcNow.AddSeconds(5)
        while(-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 200
            $process.Refresh()
            if($PromptReset -and [DateTime]::UtcNow -ge $nextReminder) {
                $remaining = [math]::Max(0, [math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
                Write-Host "Esperando DFU: pulsa RESET sin BOOT ($remaining s restantes)..." -ForegroundColor Yellow
                $nextReminder = [DateTime]::UtcNow.AddSeconds(5)
            }
        }

        if(-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit()
            Write-Host "TIMEOUT: la Daisy no reaparecio en $TimeoutSeconds s" -ForegroundColor Red
        }

        $stdout = if(Test-Path $outTmp) { Get-Content -LiteralPath $outTmp -Raw } else { '' }
        $stderr = if(Test-Path $errTmp) { Get-Content -LiteralPath $errTmp -Raw } else { '' }
        $result = ($stdout + [Environment]::NewLine + $stderr).Trim()
        if($result) { $result | Tee-Object -FilePath $log -Append | Out-Null }
        return $result
    } finally {
        Remove-Item -LiteralPath $outTmp -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $errTmp -Force -ErrorAction SilentlyContinue
    }
}

"=== FLASH DAISY $(Get-Date -Format s) ===" | Out-File -FilePath $log -Encoding utf8

# ── Verificar que el firmware existe ANTES de pedir DFU ──
if(-not (Test-Path $fw)) {
    Write-Host "No existe $fw" -ForegroundColor Red
    if($DemoBells) {
        Write-Host 'Compila la demo primero: build_daisy.ps1 -DemoBells' -ForegroundColor Yellow
    } elseif($ShowcaseDemo) {
        Write-Host 'Compila el showcase primero: build_daisy.ps1 -ShowcaseDemo -BuildDir build_showcase' -ForegroundColor Yellow
    } else {
        Write-Host 'Compila el firmware primero: build_daisy.ps1' -ForegroundColor Yellow
    }
    'RESULT=NO_FIRMWARE' | Tee-Object -FilePath $log -Append
    exit 1
}

# ── Pre-generar samples.bin ANTES de entrar en DFU (evita timeout) ──
if((-not $SkipSamples) -and (-not (Test-Path $wavblob))) {
    Write-Host 'Generando samples.bin con pack_wavs.py (antes de DFU)...' -ForegroundColor Yellow
    $genResult = Invoke-PythonScript 'pack_wavs.py' 2>&1 | Tee-Object -FilePath $log -Append | Out-String
    if(Test-Path $wavblob) {
        'RESULT=SAMPLES_BLOB_GENERATED' | Tee-Object -FilePath $log -Append
        Write-Host 'samples.bin generado correctamente' -ForegroundColor Green
    } else {
        'RESULT=SAMPLES_BLOB_MISSING' | Tee-Object -FilePath $log -Append
        Write-Host 'No se pudo generar build/samples.bin' -ForegroundColor Yellow
        if($genResult) { $genResult | Tee-Object -FilePath $log -Append | Out-Null }
    }
}

Write-Host 'PASO 1/2: Presiona BOOT + RESET en la Daisy (ROM DFU)...' -ForegroundColor Yellow

$found = $false
$bootloaderDfu = $false
for($i = 0; $i -lt 240; $i++) {
    $list = & $dfu -l 2>&1 | Out-String
    if($list -match 'df11') {
        $found = $true
        if($list -match '@Flash|name="Flash|name="QSPI') {
            $bootloaderDfu = $true
        }
        "DFU detectado en t=$($i/2)s" | Tee-Object -FilePath $log -Append
        $list | Tee-Object -FilePath $log -Append | Out-Null
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
    'RESULT=BOOTLOADER_DFU_ALREADY_ACTIVE' | Tee-Object -FilePath $log -Append
    Write-Host 'DFU del bootloader Daisy detectado: saltando flasheo de bootloader interno' -ForegroundColor Green
    Write-Host 'El bootloader dura 2 s; el flasheador se armara y pedira un RESET adicional.' -ForegroundColor Yellow
} else {
    Write-Host 'Flasheando bootloader interno...' -ForegroundColor Cyan
    $bootCandidates = @($bootPrimary)
    if(Test-Path $bootFallback) { $bootCandidates += $bootFallback }

    $bootFlashed = $false
    foreach($bootBin in $bootCandidates) {
        if(-not (Test-Path $bootBin)) { continue }
        "BOOT_TRY=$bootBin" | Tee-Object -FilePath $log -Append | Out-Null
        $resBoot = & $dfu -a 0 -s 0x08000000:leave -D $bootBin -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String
        if($resBoot -match 'Download done') {
            $bootFlashed = $true
            "RESULT=BOOT_FLASH_OK ($([System.IO.Path]::GetFileName($bootBin)))" | Tee-Object -FilePath $log -Append | Out-Null
            break
        }

        # Some STM32 DFU states report the last page as non-writeable with
        # intdfu bootloader images. Try extdfu variant automatically.
        if($resBoot -match 'Last page .* is not writeable') {
            Write-Host "Bootloader rechazado por pagina no grabable: $([System.IO.Path]::GetFileName($bootBin))" -ForegroundColor Yellow
            continue
        }
    }

    if(-not $bootFlashed) {
        'RESULT=BOOT_FLASH_FAIL' | Tee-Object -FilePath $log -Append
        Write-Host 'Fallo al flashear bootloader interno' -ForegroundColor Red
        exit 2
    }

    Write-Host 'PASO 2/2: Desconecta y reconecta USB SIN botones (bootloader DFU)...' -ForegroundColor Yellow
    $found2 = $false
    for($i = 0; $i -lt 600; $i++) {
        $list = & $dfu -l 2>&1 | Out-String
        if($list -match 'df11') {
            $found2 = $true
            "DFU bootloader detectado en t=$($i/2)s" | Tee-Object -FilePath $log -Append
            $list | Tee-Object -FilePath $log -Append | Out-Null
            break
        }
        if($i % 20 -eq 0) { Write-Host "[$($i/2)s] esperando reconexion..." -ForegroundColor DarkGray }
        Start-Sleep -Milliseconds 500
    }

    if(-not $found2) {
        'RESULT=TIMEOUT_DFU_BOOTLOADER' | Tee-Object -FilePath $log -Append
        Write-Host 'TIMEOUT: no aparecio DFU del bootloader' -ForegroundColor Red
        exit 3
    }
}

Write-Host 'Flasheando firmware app (QSPI @ 0x90040000)...' -ForegroundColor Cyan

# El firmware ocupa >256KB; dejamos 512KB de margen y los samples van a 0x900C0000
$appAddress = '0x90040000'
if($SkipSamples -or -not (Test-Path $wavblob)) {
    $appAddress = '0x90040000:leave'
}

# El listado inicial consume casi toda la ventana de 2 s. Armamos una descarga
# vigilada y pedimos un RESET cuando dfu-util ya esta esperando la enumeracion.
# En algunos intentos aparece un fallo transitorio de ERASE_PAGE/get_status;
# reintentamos automaticamente para no abortar todo el flujo por un glitch USB.
$appFlashOk = $false
$maxAppAttempts = 3
for($appTry = 1; $appTry -le $maxAppAttempts; $appTry++) {
    if($appTry -gt 1) {
        Write-Host "Reintentando firmware (intento $appTry/$maxAppAttempts)..." -ForegroundColor Yellow
        "APP_FLASH_RETRY=$appTry" | Tee-Object -FilePath $log -Append | Out-Null
    }

    $resApp = Invoke-DfuDownloadWithTimeout -Address $appAddress -FilePath $fw `
        -TimeoutSeconds 30 -PromptReset

    if($resApp -match 'Download done') {
        $appFlashOk = $true
        break
    }

    if($resApp -match 'ERASE_PAGE|get_status|LIBUSB_ERROR|No DFU capable USB device') {
        Start-Sleep -Milliseconds 500
        continue
    }

    # Error no reconocido: no merece repetir muchas veces.
    break
}

if(-not $appFlashOk) {
    'RESULT=FLASH_FAIL' | Tee-Object -FilePath $log -Append
    Write-Host 'FLASH_FAIL (firmware)' -ForegroundColor Red
    exit 4
}
Write-Host 'Firmware OK' -ForegroundColor Green

# ── Flashear WAV samples blob (QSPI @ 0x900C0000) ──
if($SkipSamples) {
    Write-Host 'SkipSamples activo: no se flashean WAV samples' -ForegroundColor Yellow
    'RESULT=SAMPLES_FLASH_SKIPPED' | Tee-Object -FilePath $log -Append
} elseif(Test-Path $wavblob) {
    $blobSizeKB = [math]::Round((Get-Item $wavblob).Length / 1KB, 1)
    Write-Host "Flasheando WAV samples ($blobSizeKB KB) a QSPI @ 0x900C0000..." -ForegroundColor Cyan
    $resWav = & $dfu -a 0 -s 0x900C0000:leave -D $wavblob -d ",0483:df11" 2>&1 | Tee-Object -FilePath $log -Append | Out-String
    if($resWav -match 'Download done') {
        "RESULT=SAMPLES_FLASH_OK ($blobSizeKB KB)" | Tee-Object -FilePath $log -Append
        Write-Host "WAV samples OK ($blobSizeKB KB)" -ForegroundColor Green
    } else {
        'RESULT=SAMPLES_FLASH_FAIL' | Tee-Object -FilePath $log -Append
        Write-Host 'WARNING: Fallo al flashear WAV samples' -ForegroundColor Yellow
    }
} else {
    Write-Host 'No se encontro build/samples.bin - sin WAV samples' -ForegroundColor Yellow
    'RESULT=NO_SAMPLES_BLOB' | Tee-Object -FilePath $log -Append
}

'RESULT=FLASH_OK' | Tee-Object -FilePath $log -Append
Write-Host 'FLASH_OK' -ForegroundColor Green
exit 0
