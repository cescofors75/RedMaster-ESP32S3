param(
    [string]$EspHost = "192.168.4.1",
    [int]$ListenPort = 8443,
    [string]$BindAddress = "0.0.0.0"
)

$ErrorActionPreference = "Stop"

function Get-PrimaryIPv4 {
    $cfg = Get-NetIPConfiguration |
        Where-Object { $_.IPv4DefaultGateway -ne $null -and $_.NetAdapter.Status -eq 'Up' } |
        Select-Object -First 1

    if ($cfg -and $cfg.IPv4Address) {
        return $cfg.IPv4Address.IPAddress
    }

    $fallback = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -ne '127.0.0.1' -and $_.PrefixOrigin -ne 'WellKnown' } |
        Select-Object -First 1

    if ($fallback) { return $fallback.IPAddress }
    return "127.0.0.1"
}

$workspace = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$toolDir = Join-Path $workspace ".tools\caddy"
$caddyExe = Join-Path $toolDir "caddy.exe"
$caddyFile = Join-Path $toolDir "Caddyfile"

New-Item -ItemType Directory -Path $toolDir -Force | Out-Null

if (-not (Test-Path $caddyExe)) {
    Write-Host "Descargando Caddy..."
    $downloadUrl = "https://caddyserver.com/api/download?os=windows&arch=amd64"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $caddyExe
}

$localIp = Get-PrimaryIPv4
$webRoot = (Join-Path $workspace "RedMaster_ESP32S3\data\web").Replace('\','/')

$caddyContent = @"
{
    admin off
    servers {
        protocols h1 h2
    }
}

https://${localIp}:${ListenPort} https://localhost:${ListenPort} {
    tls internal
    encode gzip

    root $webRoot

    handle /mobile {
        rewrite * /mobile.html
        file_server
    }

    handle /gesture {
        rewrite * /gesture.html
        file_server
    }

    handle /gesture-pro {
        rewrite * /gesture-pro.html
        file_server
    }

    handle /ws* {
        reverse_proxy http://${EspHost}:80 {
            header_up Host {upstream_hostport}
            flush_interval -1
            transport http {
                versions 1.1
                keepalive off
                dial_timeout 10s
            }
        }
    }

    handle /api/* {
        reverse_proxy http://${EspHost}:80 {
            header_up Host {upstream_hostport}
        }
    }

    handle {
        file_server
    }
}
"@

Set-Content -Path $caddyFile -Value $caddyContent -Encoding UTF8

Write-Host ""
Write-Host "Proxy HTTPS RED808 listo para iniciar"
Write-Host "ESP target: http://${EspHost}"
Write-Host "Web root  : ${webRoot}"
Write-Host "URL mobile  : https://${localIp}:${ListenPort}/mobile"
Write-Host "URL local   : https://localhost:${ListenPort}/mobile"
Write-Host "URL gesture : https://${localIp}:${ListenPort}/gesture"
Write-Host "URL gesture PRO: https://${localIp}:${ListenPort}/gesture-pro"
Write-Host ""
Write-Host "IMPORTANTE: para camara en movil, instala el certificado CA local de Caddy:"
Write-Host "  $env:APPDATA\Caddy\pki\authorities\local\root.crt"
Write-Host ""

& $caddyExe run --config $caddyFile --adapter caddyfile
