# Installs GT5 game updates and DLC from their .pkg files, for the port, by itself (no RPCS3):
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\install_pkg.ps1 <file.pkg or folder> [more ...]
#                [-Gt5 <GT5 folder>]
# (The launcher's Help page has a button for it.)
#
# 1. Each package is unpacked straight into GT5\hdd0\game\<title ID> by ps3_unlock.py (the port's own
#    tool), the way a PS3 installs it. Its own checksum is checked before anything is written. Updates go
#    in version order (1.05, 1.06 ... 2.17): each one goes on top of the one before it; ones that are
#    installed already are skipped.
# 2. update_exe.ps1 unlocks the newest update's EBOOT.BIN into GT5\input\EBOOT.elf.
# Then setup.bat (or setup.ps1) generates and builds the game for that version once (about an hour).
#
# DLC: a DLC package installs its voucher; the game still needs that licence's key in
# GT5\hdd0\home\00000001\exdata (README, DLC). Nothing here makes or holds keys.
#
# -Rpcs3 is still accepted and ignored (older launchers pass it). For testing: GT5_PKG_NOPAUSE=1 doesn't
# wait for Enter at the end.
param([Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Paths,
      [string]$Gt5 = "", [string]$Rpcs3 = "", [int]$TimeoutMinutes = 30)
$ErrorActionPreference = "Continue"
if (-not $Gt5) { $Gt5 = Split-Path $PSScriptRoot -Parent }   # (empty in this param block's defaults)
$base = Split-Path $Gt5 -Parent

function Say([string]$s, [string]$c = "Gray") { Write-Host $s -ForegroundColor $c }
function Fail([string]$s) { Say $s Yellow; if ($env:GT5_PKG_NOPAUSE -ne "1") { Read-Host "Press Enter to close" | Out-Null }; exit 2 }

# ---- The packages: content ID from each header, updates sorted by version, DLC after ----------------------
$files = @()
foreach ($p in $Paths) {
    if (-not $p) { continue }
    $p = $p.Trim('"')
    if (Test-Path -LiteralPath $p -PathType Container) { $files += @(Get-ChildItem -LiteralPath $p -Filter *.pkg -File | ForEach-Object FullName) }
    elseif (Test-Path -LiteralPath $p) { $files += (Resolve-Path -LiteralPath $p).Path }
    else { Say "  not found: $p" Yellow }
}
if (-not $files) { Fail "No .pkg files given. Usage: install_pkg.ps1 <file.pkg or folder> [more ...]" }

$pkgs = @()
foreach ($f in ($files | Select-Object -Unique)) {
    $fs = $null
    try {
        $fs = [IO.File]::OpenRead($f); $h = New-Object byte[] 0x60; [void]$fs.Read($h, 0, $h.Length)
    } catch { Say "  can't read $f" Yellow; continue } finally { if ($fs) { $fs.Close() } }
    if ($h[0] -ne 0x7F -or $h[1] -ne 0x50 -or $h[2] -ne 0x4B -or $h[3] -ne 0x47) { Say "  not a PS3 package: $f" Yellow; continue }
    $cid = [Text.Encoding]::ASCII.GetString($h, 0x30, 0x24).TrimEnd([char]0)
    $title = if ($cid.Length -ge 16) { $cid.Substring(7, 9) } else { "" }
    # Game updates are named ...-A<version>-V<pkg version>-...; their content ID ends in zeros.
    $ver = ""
    if ([IO.Path]::GetFileName($f) -match '-A(\d\d)(\d\d)-V\d{4}') { $ver = "$($Matches[1]).$($Matches[2])" }
    $isUpdate = [bool]$ver -and $cid -match '-0{16}$'
    $pkgs += [pscustomobject]@{ File = $f; Cid = $cid; Title = $title; Ver = $ver; IsUpdate = $isUpdate
                                Order = $(if ($isUpdate) { [double]$ver } else { 1000 }) }
}
if (-not $pkgs) { Fail "None of those is a PS3 package (.pkg)." }
$pkgs = @($pkgs | Sort-Object Order, File)
$titles = @($pkgs | ForEach-Object Title | Select-Object -Unique)
Say "Packages ($($pkgs.Count)):" White
foreach ($k in $pkgs) { Say ("  {0}  {1}" -f $(if ($k.IsUpdate) { "update $($k.Ver)" } else { "DLC/other " }), [IO.Path]::GetFileName($k.File)) }

# ---- The port's own tool and Python ------------------------------------------------------------------------
$tool = Join-Path $PSScriptRoot "ps3_unlock.py"
$py = @((Join-Path $base "tools\python\python.exe"), (Join-Path $base "venv\Scripts\python.exe")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $py) { $c = Get-Command python.exe -ErrorAction SilentlyContinue; if ($c) { $py = $c.Source } }
if (-not (Test-Path -LiteralPath $tool) -or -not $py) { Fail "This install is missing a part it needs to unpack packages (GT5\scripts\ps3_unlock.py, or its Python). Run Update.bat, then try again." }
$keys = Join-Path $base "tools\ps3_keys"
$hddGame = Join-Path $Gt5 "hdd0\game"
New-Item -ItemType Directory -Force $hddGame | Out-Null

function Read-Sfo([string]$path) {
    try { $b = [IO.File]::ReadAllBytes($path) } catch { return $null }
    if ($b.Length -lt 20 -or $b[1] -ne 0x50 -or $b[2] -ne 0x53 -or $b[3] -ne 0x46) { return $null }
    $keyT = [BitConverter]::ToUInt32($b, 8); $dataT = [BitConverter]::ToUInt32($b, 12)
    $n = [BitConverter]::ToUInt32($b, 16); $r = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $e = 20 + $i * 16
        if ($e + 16 -gt $b.Length) { break }
        $ko = [BitConverter]::ToUInt16($b, $e); $fmt = [BitConverter]::ToUInt16($b, $e + 2)
        $len = [BitConverter]::ToUInt32($b, $e + 4); $do = [BitConverter]::ToUInt32($b, $e + 12)
        $k = ""; $p = $keyT + $ko
        while ($p -lt $b.Length -and $b[$p] -ne 0) { $k += [char]$b[$p]; $p++ }
        if ($dataT + $do + $len -gt $b.Length) { continue }
        if ($fmt -eq 0x0404) { $r[$k] = [BitConverter]::ToUInt32($b, $dataT + $do) }
        else { $r[$k] = [Text.Encoding]::UTF8.GetString($b, $dataT + $do, $len).TrimEnd([char]0) }
    }
    return $r
}
function Installed-Ver([string]$title) {
    $s = Read-Sfo (Join-Path $hddGame "$title\PARAM.SFO")
    if ($s -and $s["APP_VER"]) { return ([string]$s["APP_VER"]).Trim() }
    return ""
}

# ---- Updates go in order: a gap is said plainly before anything is written ----------------------------------
# An update goes on top of the one before it (a player picked 2.01 to 2.11 without 1.05 to 2.00, 23 Nov 2026).
# gt5_updates.txt lists every official update in order.
$chainFile = Join-Path $PSScriptRoot "gt5_updates.txt"
if (Test-Path $chainFile) {
    $csv = @(Get-Content -LiteralPath $chainFile | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object { ,($_.Split(",")) })
    foreach ($t in $titles) {
        $chain = @($csv | Where-Object { $_[0] -eq $t } | ForEach-Object { $_[1] } | Sort-Object { [double]$_ })
        if (-not $chain) { continue }
        $have = Installed-Ver $t
        $given = @($pkgs | Where-Object { $_.IsUpdate -and $_.Title -eq $t -and (-not $have -or [double]$_.Ver -gt [double]$have) } | ForEach-Object Ver)
        if (-not $given) { continue }
        $missing = @($chain | Where-Object { (-not $have -or [double]$_ -gt [double]$have) -and [double]$_ -lt [double]$given[-1] -and $given -notcontains $_ })
        if ($missing) {
            $from = if ($have) { "$t $have is installed, so" } else { "Nothing of $t is installed yet, so" }
            Say "Updates install only in order, one after the other. $from these are missing before $($given[-1]): $($missing -join ', ')." Yellow
            Say "Pick those packages too (all of them from $($missing[0]) up to $($given[-1])), or use the launcher's Help > Game version and DLC, which downloads every update it needs from Sony." Yellow
            Fail "Nothing was installed."
        }
    }
}

# ---- Unpack each package into GT5\hdd0\game\<title ID> -----------------------------------------------------
$failed = @()
$updOk = 0   # updates that are in place now (installed in this run, or there already)
foreach ($k in $pkgs) {
    $name = [IO.Path]::GetFileName($k.File)
    if ($k.IsUpdate) {
        $have = Installed-Ver $k.Title
        if ($have -and [double]$have -ge [double]$k.Ver) { Say "  update $($k.Ver): $have is installed already, skipped"; $updOk++; continue }
    }
    Say "Installing $name ..." White
    $why = ""; $skipped = $false; $lastPct = -1
    & $py -I $tool pkg $k.File $hddGame --keys-dir $keys 2>&1 | ForEach-Object {
        $l = "$_"
        if ($l -match '^PKG_PROGRESS\s+(\d+)\s+(\d+)') {
            $pct = if ([double]$Matches[2] -gt 0) { [int]([math]::Floor(100.0 * [double]$Matches[1] / [double]$Matches[2] / 10) * 10) } else { 100 }
            if ($pct -gt $lastPct -and $pct -lt 100) { $lastPct = $pct; Say "  $pct %" }
        }
        elseif ($l -match '^PS3_UNLOCK_FAIL\s+\d+\s+\w+:\s*(.*)$') { $why = $Matches[1].Trim() }
        elseif ($l -match '^PKG_SKIP') { $skipped = $true }
    }
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        if ($skipped) { Say "  it is installed already, skipped" }
        else { Say "  installed$(if ($k.IsUpdate) { ": $($k.Title) is now at $($k.Ver)" })" Green }
        if ($k.IsUpdate) { $updOk++ }
    } else {
        Say "  not installed: $(if ($why) { $why } else { "the unpacker stopped (code $code)" })" Yellow
        $failed += $name
        if ($k.IsUpdate) { Say "  The updates after it need it, so stopping here." Yellow; break }
    }
}
foreach ($t in $titles) {
    $v = Read-Sfo (Join-Path $hddGame "$t\PARAM.SFO")
    if ($v -and $v['APP_VER']) { Say "GT5\hdd0 now holds $t version $($v['APP_VER'])." Green }
}

# ---- The update's executable --------------------------------------------------------------------------------
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "update_exe.ps1") -Gt5 $Gt5
$code = $LASTEXITCODE

Say ""
if ($failed) { Say "Not installed: $($failed -join ', ')" Yellow }
# Only when an update is in place now: with none installed nothing changed, and there is nothing to build.
if ($code -eq 2) { Say "The update's data is in place, but its executable couldn't be unlocked (see above)." Yellow }
elseif ($updOk -gt 0) { Say "Next: run setup.bat (or setup.ps1) once. It generates and builds the game for this version (about an hour)." Green }
if ($pkgs | Where-Object { -not $_.IsUpdate }) { Say "DLC: the vouchers are in place; each still needs its licence key in GT5\hdd0\home\00000001\exdata (README, DLC)." }
if ($env:GT5_PKG_NOPAUSE -ne "1") { Read-Host "Press Enter to close" | Out-Null }
exit $(if ($failed) { 1 } else { 0 })
