<#
.SYNOPSIS
  Installs pcntl for Windows into a PHP installation.

.DESCRIPTION
  1. Finds php.exe (the one on PATH, or the one you pass with -Php).
  2. Picks the matching prebuilt DLL from dist\ (built on demand with build\build.ps1
     if Visual Studio's C++ tools are installed and no prebuilt DLL matches).
  3. Copies it to the extension_dir as php_pcntl.dll.
  4. Adds "extension=pcntl" to php.ini (creating php.ini from php.ini-development
     if PHP has no ini file yet).
  5. Verifies that `php -m` lists pcntl.

.EXAMPLE
  .\install.ps1
  .\install.ps1 -Php "C:\tools\php84\php.exe"
  .\install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
	[string]$Php = "php",
	[switch]$Uninstall,
	[switch]$NoIni
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Resolve-Php([string]$candidate) {
	if (Test-Path $candidate -PathType Leaf) { return (Resolve-Path $candidate).Path }
	$cmd = Get-Command $candidate -ErrorAction SilentlyContinue
	if (-not $cmd) { throw "php executable '$candidate' not found. Pass -Php C:\path\to\php.exe" }
	$path = $cmd.Source
	if ($path -like "*.bat" -or $path -like "*.cmd") {
		$content = Get-Content $path -Raw
		if ($content -match '"([^"]+php\.exe)"') { return $Matches[1] }
	}
	return $path
}

$phpExe = Resolve-Php $Php
$probe = & $phpExe -n -f (Join-Path $root "build\probe.php")
$p = "$probe".Trim().Split("|")
$version = $p[0]; $ts = $p[2]; $mm = $p[3]; $buildId = $p[4]
$arch = if ([int]$p[1] -eq 64) { "x64" } else { "x86" }
Write-Host "PHP $version $($ts.ToUpper()) $arch at $phpExe"

if ([int]$version.Split(".")[0] -lt 8 -or ($mm -eq "8.0" -or $mm -eq "8.1")) {
	throw "pcntl for Windows needs PHP 8.2 or newer (found $version)."
}

# where does this PHP load extensions from, and which ini does it use?
$iniProbe = ("$(& $phpExe -f (Join-Path $root 'build\probe-ini.php') 2>$null)").Trim().Split("|")
$extDir = $iniProbe[0]
if (-not $extDir) { $extDir = "ext" }
if (-not [System.IO.Path]::IsPathRooted($extDir)) { $extDir = Join-Path (Split-Path $phpExe) $extDir }
$iniFile = if ($iniProbe.Count -gt 1) { $iniProbe[1] } else { "" }
$target = Join-Path $extDir "php_pcntl.dll"

if ($Uninstall) {
	if (Test-Path $target) { Remove-Item $target -Force; Write-Host "Removed $target" }
	if ($iniFile -and (Test-Path $iniFile)) {
		$lines = Get-Content $iniFile
		$kept = $lines | Where-Object { $_ -notmatch '^\s*extension\s*=\s*"?(php_)?pcntl(\.dll)?"?\s*(;.*)?$' -and $_ -notmatch '^; pcntl for Windows' }
		if ($kept.Count -ne $lines.Count) { Set-Content $iniFile $kept -Encoding ascii; Write-Host "Removed extension=pcntl from $iniFile" }
	}
	exit 0
}

# pick or build the DLL
$dll = Join-Path $root "dist\php_pcntl-$mm-$ts-$arch.dll"
if (-not (Test-Path $dll)) {
	Write-Host "No prebuilt DLL for PHP $mm $ts $arch; trying to build one..."
	& (Join-Path $root "build\build.ps1") -Php $phpExe -Out $dll
	if (-not (Test-Path $dll)) { throw "Build failed" }
}

# make sure it actually loads into this exact PHP before touching anything
$check = & $phpExe -n -d "extension=$dll" -f (Join-Path $root "build\check.php") 2>&1
if ("$check" -notmatch "^loaded") {
	throw "The DLL did not load into $phpExe (build id $buildId): $check`nRun .\build\build.ps1 -Php `"$phpExe`" to build one for this exact PHP."
}

New-Item -ItemType Directory -Force $extDir | Out-Null
if (Test-Path $target) { Copy-Item $target "$target.bak" -Force }
Copy-Item $dll $target -Force
Write-Host "Installed $target"

if (-not $NoIni) {
	if (-not $iniFile) {
		$phpDir = Split-Path $phpExe
		$iniFile = Join-Path $phpDir "php.ini"
		$template = Join-Path $phpDir "php.ini-development"
		if (-not (Test-Path $iniFile)) {
			if (Test-Path $template) { Copy-Item $template $iniFile } else { New-Item -ItemType File $iniFile | Out-Null }
			Write-Host "Created $iniFile"
		}
	}
	$content = if (Test-Path $iniFile) { Get-Content $iniFile -Raw } else { "" }
	if ($content -match '(?m)^\s*extension\s*=\s*"?(php_)?pcntl(\.dll)?"?\s*(;.*)?$') {
		Write-Host "php.ini already enables pcntl ($iniFile)"
	} else {
		Add-Content $iniFile "`r`n; pcntl for Windows - added by install.ps1`r`nextension=pcntl`r`n" -Encoding ascii
		Write-Host "Added extension=pcntl to $iniFile"
	}
}

$loaded = (& $phpExe -f (Join-Path $root 'build\check.php') 2>&1)
if ("$loaded" -match "^loaded") {
	Write-Host "`npcntl is enabled. Try:  $phpExe -r `"pcntl_signal(SIGINT, fn() => print('bye'.PHP_EOL) && exit(0)); echo 'press Ctrl+C', PHP_EOL; while (true) { pcntl_signal_dispatch(); usleep(100000); }`"" -ForegroundColor Green
} else {
	Write-Warning "php_pcntl.dll was copied but `php -r 'extension_loaded(\"pcntl\")'` still says no. Output was: $loaded"
	Write-Warning "Check that $iniFile is the ini file your PHP actually loads (php --ini)."
}
