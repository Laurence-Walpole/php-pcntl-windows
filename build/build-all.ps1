<#
.SYNOPSIS
  Builds and tests php_pcntl.dll for every php.exe given (or every Laravel Herd PHP 8.x found).

.EXAMPLE
  .\build\build-all.ps1
  .\build\build-all.ps1 -Php C:\php82\php.exe, C:\php84\php.exe
#>
param(
	[string[]]$Php,
	[switch]$NoTests
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Php) {
	$herd = Join-Path $env:USERPROFILE ".config\herd\bin"
	$Php = Get-ChildItem $herd -Directory -Filter "php8*" -ErrorAction SilentlyContinue |
		ForEach-Object { Join-Path $_.FullName "php.exe" } | Where-Object { Test-Path $_ }
	if (-not $Php) { throw "No php.exe given and no Laravel Herd PHP 8.x installations found" }
}

$results = @()
foreach ($exe in $Php) {
	Write-Host "`n=== $exe ===" -ForegroundColor Cyan
	$ok = $true
	try {
		& (Join-Path $PSScriptRoot "build.ps1") -Php $exe
	} catch {
		Write-Host $_ -ForegroundColor Red
		$ok = $false
	}
	$tests = "skipped"
	if ($ok -and -not $NoTests) {
		& $exe (Join-Path $root "tests\run.php") 2>&1 | Where-Object { $_ -notmatch "^\s+ok " }
		$tests = if ($LASTEXITCODE -eq 0) { "passed" } else { "FAILED" }
	}
	$results += [pscustomobject]@{ Php = $exe; Build = if ($ok) { "ok" } else { "FAILED" }; Tests = $tests }
}
Write-Host "`nSummary:" -ForegroundColor Cyan
$results | Format-Table -AutoSize
if ($results | Where-Object { $_.Build -ne "ok" -or $_.Tests -eq "FAILED" }) { exit 1 }
