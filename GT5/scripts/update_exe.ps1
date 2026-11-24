# Makes GT5\input\EBOOT.elf match the game update installed in GT5\hdd0. setup.ps1 and update.ps1 run it
# before the game code is generated:
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\update_exe.ps1 [-Gt5 <GT5 folder>]
#
# GT5 reads updated files from the hard disk whatever its own version, so the disc's 01.00 executable
# with an update's data runs the update's newer scripts, which call code 01.00 doesn't have: the game
# stops before the main menu with a black screen (13 Nov 2026, US 2.11 with the Master Mod). An update
# is installed when GT5\hdd0\game\<title ID> holds its PARAM.SFO (APP_VER newer than 01.00) or its
# EBOOT.BIN; the disc version writes neither there.
#
# When GT5\input\EBOOT.elf is a disc's 01.00 and an update is installed, this finds the update's
# EBOOT.BIN (in GT5\hdd0, where install_pkg.ps1 puts it; else in an RPCS3 folder's dev_hdd0, for data that
# was copied over from RPCS3 by hand), unlocks it with the port's own tool (ps3_unlock.py, as the installer
# does for the disc's) and puts the result in GT5\input\EBOOT.elf. The disc's one is kept as
# GT5\input\EBOOT.disc-01.00.elf. RPCS3 itself is never started; its folder is only looked in for the file
# (GT5_RPCS3, the old tools\rpcs3, PATH, and a few usual folders).
#
# Exit codes: 0 nothing to do, or switched; 2 an update is installed but its executable couldn't be
# made (the message says what to do by hand).
param([string]$Gt5 = (Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference = "Continue"
$base = Split-Path $Gt5 -Parent
$elf = Join-Path $Gt5 "input\EBOOT.elf"

# The discs' own executables (version 01.00): Europe, North America, Hong Kong / Asia.
$discSha = @{
    "1535B865EC7DD58E7704B378B8621532ECF844E1" = "Europe BCES00569"
    "40126D2D14B0A95B9601BB11E3A6E778186067AC" = "North America BCUS98114"
    "49A3662D4B541BB4E0C2E72D2602F89FEB710E71" = "Hong Kong / Asia BCAS20108"
}

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
function App-Ver([string]$dir) {
    $s = Read-Sfo (Join-Path $dir "PARAM.SFO")
    if ($s -and $s["APP_VER"]) { return ([string]$s["APP_VER"]).Trim() }
    return ""
}
function Is-Newer([string]$v) { return $v -and $v -notmatch '^0?1\.00$' }

if (-not (Test-Path $elf)) { exit 0 }
$sha = (Get-FileHash $elf -Algorithm SHA1).Hash
if (-not $discSha.ContainsKey($sha)) { exit 0 }   # already an update's (or another) executable

# An update installed in GT5\hdd0?
$upd = $null
foreach ($gd in @(Get-ChildItem (Join-Path $Gt5 "hdd0\game") -Directory -ErrorAction SilentlyContinue)) {
    $v = App-Ver $gd.FullName
    if ((Is-Newer $v) -or (Test-Path (Join-Path $gd.FullName "USRDIR\EBOOT.BIN"))) { $upd = @{ Id = $gd.Name; Dir = $gd.FullName; Ver = $v }; break }
}
if (-not $upd) { exit 0 }
$what = if ($upd.Ver) { "update $($upd.Ver)" } else { "a game update" }
Write-Host "  GT5\hdd0\game\$($upd.Id) holds $what's data, but GT5\input\EBOOT.elf is the disc's version 01.00 ($($discSha[$sha]))."
Write-Host "  The game needs the update's executable too (with the disc's it stops before the main menu)."

# RPCS3 folders, only as places the update's EBOOT.BIN may be in.
$rpcs3 = @()
if ($env:GT5_RPCS3) { $rpcs3 += $(if ($env:GT5_RPCS3 -like "*.exe") { $env:GT5_RPCS3 } else { Join-Path $env:GT5_RPCS3 "rpcs3.exe" }) }
$rpcs3 += Join-Path $base "tools\rpcs3\rpcs3.exe"
$onPath = Get-Command rpcs3.exe -ErrorAction SilentlyContinue
if ($onPath) { $rpcs3 += $onPath.Source }
$roots = @("$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Documents", "$env:LOCALAPPDATA",
           "$env:ProgramFiles", "${env:ProgramFiles(x86)}")
$roots += @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
    $_.Root; Join-Path $_.Root "Games"; Join-Path $_.Root "Emulators"; Join-Path $_.Root "Downloads" })
foreach ($r in $roots) {
    if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
    foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -Filter "*rpcs3*" -ErrorAction SilentlyContinue)) {
        $rpcs3 += Join-Path $d.FullName "rpcs3.exe"
        foreach ($s in @(Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue)) { $rpcs3 += Join-Path $s.FullName "rpcs3.exe" }
    }
}
$rpcs3 = @($rpcs3 | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)

# The update's EBOOT.BIN: in GT5\hdd0 first, else in each RPCS3's dev_hdd0 (its config\vfs.yml can move it).
$cands = @()
$own = Join-Path $upd.Dir "USRDIR\EBOOT.BIN"
if (Test-Path $own) { $cands += $own }
foreach ($x in $rpcs3) {
    $emu = Split-Path $x -Parent
    $hdd = Join-Path $emu "dev_hdd0"
    $vfs = Join-Path $emu "config\vfs.yml"
    if (Test-Path $vfs) {
        $line = Get-Content $vfs -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\s*/dev_hdd0/\s*:' } | Select-Object -First 1
        if ($line) {
            $p = ($line -replace '^\s*/dev_hdd0/\s*:\s*', '').Trim().Trim('"').Replace('$(EmulatorDir)', "$emu\")
            if ($p) { $hdd = $p.TrimEnd('/', '\') }
        }
    }
    $g = Join-Path $hdd "game\$($upd.Id)"
    $b = Join-Path $g "USRDIR\EBOOT.BIN"
    if (-not (Test-Path $b)) { continue }
    # The same update as ours, when both PARAM.SFO say which.
    $v = App-Ver $g
    if ($upd.Ver -and $v -and $v -ne $upd.Ver) { Write-Host "  (RPCS3's $g is update $v, not $($upd.Ver): not used)"; continue }
    $cands += $b
}
$cands = @($cands | Select-Object -Unique)

$manual = "Put the update's EBOOT.BIN (it is in the update's package; the launcher's Help > Game version and DLC installs the whole update) " +
          "into GT5\hdd0\game\$($upd.Id)\USRDIR and run this again. To play the disc version " +
          "instead, move the folder GT5\hdd0\game\$($upd.Id) somewhere else (your saves are in GT5\gamedata, not there)."
if (-not $cands) {
    Write-Host "  Couldn't find the update's EBOOT.BIN (not in GT5\hdd0\game\$($upd.Id)\USRDIR)." -ForegroundColor Yellow
    Write-Host "  $manual" -ForegroundColor Yellow
    exit 2
}

# The port's own unlock tool, on the port's own Python.
$tool = Join-Path $PSScriptRoot "ps3_unlock.py"
$py = @((Join-Path $base "tools\python\python.exe"), (Join-Path $base "venv\Scripts\python.exe")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $py) { $c = Get-Command python.exe -ErrorAction SilentlyContinue; if ($c) { $py = $c.Source } }
$why = ""
$work = Join-Path $Gt5 "input\decrypt_tmp"
foreach ($bin in $cands) {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item -LiteralPath $bin (Join-Path $work "EBOOT.BIN")
    $out = Join-Path $work "EBOOT.elf"
    $head = [IO.File]::ReadAllBytes((Join-Path $work "EBOOT.BIN"))[0..3]
    if ($head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46) {
        Move-Item (Join-Path $work "EBOOT.BIN") $out -Force   # already unlocked (some mods ship it so)
    } elseif ($py -and (Test-Path -LiteralPath $tool)) {
        Write-Host "  unlocking $bin ..."
        & $py -I $tool eboot (Join-Path $work "EBOOT.BIN") $out --keys-dir (Join-Path $base "tools\ps3_keys") 2>&1 | ForEach-Object {
            if ("$_" -match '^PS3_UNLOCK_FAIL\s+\d+\s+\w+:\s*(.*)$') { $why = $Matches[1].Trim() }
        }
    }
    if ((Test-Path $out) -and ([IO.File]::ReadAllBytes($out)[0] -eq 0x7F)) {
        $newSha = (Get-FileHash $out -Algorithm SHA1).Hash
        if ($discSha.ContainsKey($newSha)) { Write-Host "  $bin is the disc's own executable: skipped"; continue }
        $keep = Join-Path $Gt5 "input\EBOOT.disc-01.00.elf"
        if (-not (Test-Path $keep)) { Copy-Item $elf $keep }
        Move-Item $out $elf -Force
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
        Write-Host "  GT5\input\EBOOT.elf is now $what's executable (SHA-1 $newSha), from $bin." -ForegroundColor Green
        Write-Host "  The disc's is kept as GT5\input\EBOOT.disc-01.00.elf. The game code is generated again for it."
        exit 0
    }
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if (-not $py -or -not (Test-Path -LiteralPath $tool)) { Write-Host "  This install is missing the part that unlocks it (GT5\scripts\ps3_unlock.py, or its Python): run Update.bat, then try again." -ForegroundColor Yellow }
else { Write-Host "  Unlocking the update's EBOOT.BIN didn't work$(if ($why) { " ($why)" })." -ForegroundColor Yellow }
Write-Host "  $manual" -ForegroundColor Yellow
exit 2
