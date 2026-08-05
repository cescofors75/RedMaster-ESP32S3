param(
    [string]$ProjectDir = (Join-Path $PSScriptRoot 'DaisySeed'),
    [string]$ToolchainBin,
    [string]$MakeBin,
    [switch]$Clean,
    [switch]$VerboseBuild,
    [switch]$StressReport,
    [int]$StressSeconds = 18,
    [switch]$DemoBells,
    [switch]$BootProgressDiag,
    [switch]$ShowcaseDemo,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

# Keep presentation builds isolated from the normal firmware. This also makes
# `build_daisy.ps1 -ShowcaseDemo` produce the path expected by flash_daisy.ps1.
if($ShowcaseDemo -and -not $BuildDir) {
    $BuildDir = 'build_showcase'
}

function Find-FirstExistingFile {
    param([string[]]$Patterns)
    foreach($pattern in $Patterns) {
        $match = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if($match) { return $match.FullName }
    }
    return $null
}

function Resolve-ToolDir {
    param(
        [string]$ExplicitDir,
        [string[]]$ExecutablePatterns,
        [string[]]$CommandNames,
        [string]$FriendlyName
    )

    if($ExplicitDir) {
        foreach($commandName in $CommandNames) {
            if(Test-Path (Join-Path $ExplicitDir $commandName)) { return (Resolve-Path $ExplicitDir).Path }
        }
        throw "$FriendlyName no encontrado en: $ExplicitDir"
    }

    foreach($commandName in $CommandNames) {
        $fromPath = Get-Command $commandName -ErrorAction SilentlyContinue
        if($fromPath) { return Split-Path $fromPath.Source -Parent }
    }

    $exe = Find-FirstExistingFile $ExecutablePatterns
    if($exe) { return Split-Path $exe -Parent }

    throw "$FriendlyName no encontrado. Instala STM32CubeIDE/ARM GNU Toolchain o pasa -ToolchainBin/-MakeBin."
}

$armPatterns = @(
    'C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin\arm-none-eabi-gcc.exe',
    'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin\arm-none-eabi-gcc.exe',
    'C:\Program Files*\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-gcc.exe',
    'C:\Program Files*\GNU Arm Embedded Toolchain\*\bin\arm-none-eabi-gcc.exe'
)

$makePatterns = @(
    'C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.*\tools\bin\make.exe',
    'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.*\tools\bin\make.exe',
    'C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.*\tools\bin\mingw32-make.exe',
    'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.*\tools\bin\mingw32-make.exe'
)

$gccDir = Resolve-ToolDir -ExplicitDir $ToolchainBin -ExecutablePatterns $armPatterns -CommandNames @('arm-none-eabi-gcc.exe') -FriendlyName 'arm-none-eabi-gcc'
$makeDir = Resolve-ToolDir -ExplicitDir $MakeBin -ExecutablePatterns $makePatterns -CommandNames @('make.exe', 'mingw32-make.exe') -FriendlyName 'make'
$env:PATH = "$gccDir;$makeDir;$env:PATH"

$makeCmd = Get-Command make.exe -ErrorAction SilentlyContinue
if(-not $makeCmd) { $makeCmd = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue }
if(-not $makeCmd) { throw 'make.exe/mingw32-make.exe no disponible despues de preparar PATH.' }

if(-not (Test-Path (Join-Path $ProjectDir 'Makefile'))) {
    throw "No se encontro Makefile en $ProjectDir"
}

Write-Host "Daisy Project: $ProjectDir" -ForegroundColor Cyan
Write-Host "ARM GCC:       $gccDir" -ForegroundColor Cyan
Write-Host "Make:          $($makeCmd.Source)" -ForegroundColor Cyan

Push-Location $ProjectDir
try {
    $makeArgs = @()
    if($BuildDir)         { $makeArgs += "BUILD_DIR=$BuildDir" }
    if($DemoBells)        { $makeArgs += 'DEMO_BELLS=1' }
    if($VerboseBuild)     { $makeArgs += 'VERBOSE=1' }
    if($BootProgressDiag) { $makeArgs += 'RED808_BOOT_PROGRESS_DIAG=1' }
    if($ShowcaseDemo)     { $makeArgs += 'RED808_STARTUP_SHOWCASE_DEMO=1' }
    if($StressReport) {
        $makeArgs += 'RED808_STARTUP_STRESS_REPORT=1'
        $makeArgs += "RED808_STARTUP_STRESS_SECONDS=$StressSeconds"
    }

    # Make does not track command-line macro changes. Always clean dedicated
    # diagnostic/demo profiles so a previous main.o cannot silently be reused.
    $profileBuild = $DemoBells -or $BootProgressDiag -or $ShowcaseDemo -or $StressReport
    if($Clean -or $profileBuild) {
        $cleanArgs = @('clean')
        if($BuildDir) { $cleanArgs += "BUILD_DIR=$BuildDir" }
        if($profileBuild) {
            Write-Host 'Limpiando objetos del perfil para aplicar sus macros...' -ForegroundColor Yellow
        }
        & $makeCmd.Source @cleanArgs
        if($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    & $makeCmd.Source @makeArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
