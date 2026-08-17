param(
    [int]$Baud = 115200,
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'

if(-not (Test-Path $pio)) {
    throw "No se encontro PlatformIO en $pio"
}

$devices = @(& $pio device list --json-output | ConvertFrom-Json)
if($LASTEXITCODE -ne 0) {
    throw "PlatformIO no pudo enumerar los puertos serie (codigo $LASTEXITCODE)."
}

if($Port) {
    $daisy = $devices | Where-Object { $_.port -eq $Port } | Select-Object -First 1
    if(-not $daisy) {
        throw "El puerto $Port no esta disponible."
    }
} else {
    $matches = @($devices | Where-Object {
        ([string]$_.hwid -match 'VID:PID=0483:5740') -or
        ([string]$_.description -match 'Daisy Seed|Electrosmith')
    })

    if($matches.Count -eq 0) {
        $dfuDevice = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { [string]$_.InstanceId -match '^USB\\VID_0483&PID_DF11' } |
            Select-Object -First 1

        if($dfuDevice) {
            Write-Host 'DaisyPod detectada, pero esta en modo DFU (0483:DF11).' -ForegroundColor Yellow
            Write-Host '1) Suelta por completo el boton BOOT de la Daisy Seed.' -ForegroundColor Cyan
            Write-Host '2) Pulsa y suelta RESET una vez, sin mantener BOOT.' -ForegroundColor Cyan
            Write-Host '3) Espera a que arranque RayDrone y vuelve a ejecutar esta tarea.' -ForegroundColor Cyan
            Write-Host 'Si reaparece DFU, comprueba que BOOT no este atascado o pulsado.' -ForegroundColor DarkYellow
            exit 1
        }

        Write-Host 'No se detecto DaisyPod USB CDC (VID:PID 0483:5740).' -ForegroundColor Red
        Write-Host 'Conecta el USB de Daisy Seed directamente al PC; no puede estar conectado a la P4 al mismo tiempo.' -ForegroundColor Yellow
        if($devices.Count -gt 0) {
            Write-Host 'Puertos disponibles:' -ForegroundColor DarkGray
            $devices | ForEach-Object {
                Write-Host ("  {0}  {1}  [{2}]" -f $_.port, $_.description, $_.hwid)
            }
        }
        exit 1
    }

    if($matches.Count -gt 1) {
        Write-Host 'Hay mas de una Daisy conectada. Ejecuta el script con -Port COMx:' -ForegroundColor Yellow
        $matches | ForEach-Object { Write-Host ("  {0}  {1}" -f $_.port, $_.description) }
        exit 1
    }

    $daisy = $matches[0]
}

Write-Host ("Monitor DaisyPod en {0} @ {1}" -f $daisy.port, $Baud) -ForegroundColor Green
& $pio device monitor --port $daisy.port --baud $Baud
exit $LASTEXITCODE