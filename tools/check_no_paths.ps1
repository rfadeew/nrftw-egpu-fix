# Fails (exit 1) if any file in the given folder contains a local path or the
# current user name, as ASCII or UTF-16LE. Used by package.bat on the release files.
param([Parameter(Mandatory)][string]$Dir)

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$patterns = @('\Users\', '/Users/', 'SteamLibrary', $repo, $env:USERNAME, $env:COMPUTERNAME) |
    Where-Object { $_ -and $_.Length -ge 4 } | Select-Object -Unique

$bad = 0
foreach ($f in Get-ChildItem -LiteralPath $Dir -File -Recurse) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $ascii = [Text.Encoding]::GetEncoding(28591).GetString($bytes)   # 1 byte = 1 char
    $utf16 = [Text.Encoding]::Unicode.GetString($bytes)
    foreach ($p in $patterns) {
        foreach ($text in @($ascii, $utf16)) {
            if ($text.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                Write-Host "LOCAL PATH: '$p' found in $($f.Name)"
                $bad++
                break
            }
        }
    }
}
if ($bad) { exit 1 }
Write-Host "Path check OK: no local paths in $((Get-ChildItem -LiteralPath $Dir -File -Recurse).Count) files"
