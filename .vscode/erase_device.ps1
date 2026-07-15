param(
    [Parameter(Mandatory=$true)][ValidateSet('p4','s3','c6')][string]$Device,
    [int]$Baud = 115200,
    [string]$Port
)

$pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
$py = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'

$chipArg = switch ($Device) {
    'p4' { 'esp32p4' }
    's3' { 'esp32s3' }
    'c6' { 'esp32c6' }
}

$expectedChip = switch ($Device) {
    'p4' { 'ESP32-P4' }
    's3' { 'ESP32-S3' }
    'c6' { 'ESP32-C6' }
}

$vidPids = switch ($Device) {
    'p4' { @('303A:1001') }
    's3' { @('1A86:55D3') }
    'c6' { @('1A86:55D3', '303A:1001') }
}

$descPattern = switch ($Device) {
    'p4' { 'P4|ESP32-P4' }
    's3' { 'S3|ESP32-S3|CH343|CH340|USB-Serial' }
    'c6' { 'C6|ESP32-C6|CH343|USB-Enhanced-SERIAL|USB JTAG' }
}

Write-Host "Buscando dispositivo $Device..." -ForegroundColor Cyan
$json = & $pio device list --json-output
$devs = $json | ConvertFrom-Json

function Get-DetectedChip([string]$comPort) {
    try {
        $out = & $py -m esptool --port $comPort chip_id 2>&1 | Out-String
        if ($out -match 'Connected to\s+([^\s]+)\s+on') {
            return $Matches[1].Trim()
        }
        if ($out -match 'Chip type:\s*([^\r\n]+)') {
            return $Matches[1].Trim()
        }
        if ($out -match 'Detecting chip type\.\.\.\s*([^\r\n]+)') {
            return $Matches[1].Trim()
        }
        if ($out -match 'This chip is\s*([^,\r\n]+)') {
            return $Matches[1].Trim()
        }
        return ''
    } catch {
        return ''
    }
}

function Is-ExpectedChip([string]$chipName) {
    if ([string]::IsNullOrWhiteSpace($chipName)) { return $false }
    return $chipName -match [Regex]::Escape($expectedChip)
}

if ($Port) {
    $found = $devs | Where-Object { $_.port -eq $Port } | Select-Object -First 1
    if (-not $found) {
        Write-Host "ERROR: Puerto $Port no encontrado para $Device." -ForegroundColor Red
        Write-Host "Puertos disponibles:" -ForegroundColor Yellow
        $devs | ForEach-Object { Write-Host ("  " + $_.port + "  " + $_.description + "  [" + $_.hwid + "]") }
        exit 1
    }
} else {
    $candidates = $devs | Where-Object {
        $hwid = [string]$_.hwid
        $desc = [string]$_.description
        (($vidPids | ForEach-Object { "VID:PID=$_" }) | Where-Object { $hwid -match $_ }).Count -gt 0 -or
        ($desc -match $descPattern)
    }

    if (-not $candidates) {
        Write-Host "Aviso: sin match preliminar por VID/descripcion, probando puertos por deteccion real de chip..." -ForegroundColor Yellow
        $candidates = $devs
    }

    if ($candidates.Count -eq 1) {
        $found = $candidates | Select-Object -First 1
    } else {
        foreach ($candidate in $candidates) {
            $chip = Get-DetectedChip $candidate.port
            if (Is-ExpectedChip $chip) {
                $found = $candidate
                break
            }
        }
    }
}

if (-not $found) {
    $fallbackCandidates = $devs | Where-Object {
        $desc = [string]$_.description
        $hwid = [string]$_.hwid
        switch ($Device) {
            'p4' { ($desc -match 'CH343|CH340|USB-Enhanced-SERIAL|USB-Serial') -or ($hwid -match 'VID:PID=1A86:55D3|VID:PID=303A:1001') }
            's3' { ($desc -match 'CH343|CH340|USB-Enhanced-SERIAL|USB-Serial') -or ($hwid -match 'VID:PID=1A86:55D3|VID:PID=303A:1001') }
            'c6' { ($desc -match 'CH343|CH340|USB-Enhanced-SERIAL|USB-Serial|USB JTAG') -or ($hwid -match 'VID:PID=1A86:55D3|VID:PID=303A:1001') }
        }
    }

    if ($fallbackCandidates.Count -eq 1) {
        $found = $fallbackCandidates | Select-Object -First 1
        Write-Host ("Aviso: no se pudo verificar el chip por esptool; usando puerto serie unico plausible " + $found.port + ".") -ForegroundColor Yellow
    }
}

if (-not $found) {
    Write-Host ("ERROR: No se encontro un puerto con chip '" + $expectedChip + "' para $Device.") -ForegroundColor Red
    Write-Host "Puertos disponibles:" -ForegroundColor Yellow
    $devs | ForEach-Object { Write-Host ("  " + $_.port + "  " + $_.description + "  [" + $_.hwid + "]") }
    exit 1
}

Write-Host ("Borrando flash en " + $found.port + " (" + $found.description + ") @ " + $Baud) -ForegroundColor Green

& $py -m esptool --chip $chipArg --port $found.port --baud $Baud erase-flash
exit $LASTEXITCODE
