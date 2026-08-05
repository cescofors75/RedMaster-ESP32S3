$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$master = Join-Path $root 'RedMaster_ESP32S3\src\usb_transport_protocol.h'
$p4 = Join-Path $root 'BlueSlaveP4\src\usb_transport_protocol.h'

if (-not (Test-Path -LiteralPath $master) -or -not (Test-Path -LiteralPath $p4)) {
    throw 'Missing USB transport protocol header'
}

$masterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $master).Hash
$p4Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $p4).Hash
if ($masterHash -ne $p4Hash) {
    throw 'USB transport protocol headers differ between Master and P4'
}

Write-Host 'USB transport protocol headers are synchronized.'
