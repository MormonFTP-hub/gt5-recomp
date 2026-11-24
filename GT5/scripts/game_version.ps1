# Takes the game to the version you choose: the disc's 1.00 or any official update up to 2.17 (the Master Mod
# needs exactly 2.11). The launcher's Help > Game version runs it; by hand:
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\game_version.ps1 -To 2.11 [-Gt5 <GT5 folder>]
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\game_version.ps1 -Show
#
# The updates come from Sony's own update server, the same packages a PS3 gets. gt5_updates.txt beside
# this script pins each one (address, size and SHA-1, as Sony's update list gives them, read 23 Nov 2026): a
# download is used only when it is exactly that file, so the plain-HTTP address can't be used to slip anything
# else in. They're kept in <install>\downloads\game-updates and used again for the next change.
#
#   newer than now: the missing updates are downloaded and installed by install_pkg.ps1 (the port's own
#                   unpacker; no RPCS3), then the game is converted and built again.
#   older than now: the updates up to the one chosen are installed again from 1.05 on.
#   1.00:           the update data goes and the disc's executable comes back.
# Either way the installed update data (GT5\hdd0\game\<title ID>) and executable are set aside first, deleted once
# the change has worked and put back if it didn't.
# Saves are not touched (they are in GT5\gamedata). Not while a game mod is installed (remove it first: an update
# would overwrite part of it). DLC isn't changed either way.
#
# -Show prints what the launcher's window needs: title, installed version, a mod or not, and for each version
# the megabytes still to download to reach it. Exit codes: 0 done (or nothing to do), 2 not done.
param([string]$To = "", [switch]$Show, [string]$Gt5 = "", [string]$Cache = "")
$ErrorActionPreference = "Continue"
if (-not $Gt5) { $Gt5 = Split-Path $PSScriptRoot -Parent }
$base = Split-Path $Gt5 -Parent
if (-not $Cache) { $Cache = Join-Path $base "downloads\game-updates" }

function Say([string]$s, [string]$c = "Gray") { Write-Host $s -ForegroundColor $c }
function Fail([string]$s) { Say $s Yellow; exit 2 }

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
function Norm-Ver([string]$v) {
    $v = $v.Trim()
    if ($v -match '^(\d{1,2})\.(\d{2})$') { return ("{0:00}.{1}" -f [int]$Matches[1], $Matches[2]) }
    return $v
}
function Show-Ver([string]$v) { return ($v -replace '^0', '') }

# ---- The game, its installed version and the updates Sony has for it ------------------------------------
$sfo = Read-Sfo (Join-Path $Gt5 "vfs\PS3_GAME\PARAM.SFO")
$title = if ($sfo -and $sfo["TITLE_ID"]) { ([string]$sfo["TITLE_ID"]).Trim() } else { "" }
$table = @()
foreach ($l in (Get-Content -LiteralPath (Join-Path $PSScriptRoot "gt5_updates.txt") -ErrorAction SilentlyContinue)) {
    if (-not $l -or $l.StartsWith("#")) { continue }
    $f = $l.Split(",")
    if ($f.Count -lt 5) { continue }
    $table += [pscustomobject]@{ Title = $f[0]; Ver = $f[1]; Size = [int64]$f[2]; Sha = $f[3].ToLower(); Url = $f[4]
                                 File = [IO.Path]::GetFileName($f[4]) }
}
if (-not $title) {   # no disc PARAM.SFO: the update data's folder says which game it is
    foreach ($gd in @(Get-ChildItem (Join-Path $Gt5 "hdd0\game") -Directory -ErrorAction SilentlyContinue)) {
        if ($table | Where-Object { $_.Title -eq $gd.Name }) { $title = $gd.Name; break }
    }
}
$rows = @($table | Where-Object { $_.Title -eq $title } | Sort-Object { [double]$_.Ver })
$gameDir = Join-Path $Gt5 "hdd0\game\$title"
$cur = "01.00"
$s = Read-Sfo (Join-Path $gameDir "PARAM.SFO")
if ($s -and $s["APP_VER"]) { $cur = Norm-Ver ([string]$s["APP_VER"]) }
$modOn = Test-Path (Join-Path $gameDir "USRDIR\mod_installed.txt")

# Bytes still to download to reach a version from here (the packages already kept count as there).
function Have-Pkg($r) { $p = Join-Path $Cache $r.File; return (Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).Length -eq $r.Size }
function Needed([string]$to) {
    if ($to -eq "01.00") { return @() }
    if ([double]$to -ge [double]$cur -and $cur -ne "01.00") { return @($rows | Where-Object { [double]$_.Ver -gt [double]$cur -and [double]$_.Ver -le [double]$to }) }
    return @($rows | Where-Object { [double]$_.Ver -le [double]$to })
}

if ($Show) {
    Write-Output "title=$title"
    Write-Output "installed=$cur"
    Write-Output "mod=$(if ($modOn) { 1 } else { 0 })"
    if ($rows) {
        Write-Output "version=01.00 mb=0"
        foreach ($r in $rows) {
            $mb = 0; foreach ($n in (Needed $r.Ver)) { if (-not (Have-Pkg $n)) { $mb += $n.Size } }
            Write-Output ("version={0} mb={1}" -f $r.Ver, [math]::Ceiling($mb / 1MB))
        }
    }
    exit 0
}

if (-not $title) { Fail "Couldn't tell which GT5 this is (no GT5\vfs\PS3_GAME\PARAM.SFO)." }
if (-not $rows) { Fail "There are no official updates listed for $title (gt5_updates.txt has Europe BCES00569, North America BCUS98114, Asia BCAS20108 and Japan BCJS30001)." }
if (-not $To) { Fail "Which version? e.g. -To 2.11 (versions: 1.00, $((($rows | ForEach-Object { Show-Ver $_.Ver }) -join ', ')))." }
$want = switch -regex ($To.Trim().ToLower()) { '^(newest|latest)$' { $rows[-1].Ver } '^(disc|1\.0+|01\.00)$' { "01.00" } default { Norm-Ver $To } }
if ($want -ne "01.00" -and -not ($rows | Where-Object { $_.Ver -eq $want })) { Fail "$title has no update $(Show-Ver $want). Versions: 1.00, $((($rows | ForEach-Object { Show-Ver $_.Ver }) -join ', '))." }
Say "GT5 $title is at version $(Show-Ver $cur); going to $(Show-Ver $want)." White
if ($want -eq $cur) { Say "It's already at $(Show-Ver $want): nothing to do." Green; exit 0 }
if ($modOn) { Fail "A game mod is installed (Help > Remove the game mod first): a different version would overwrite part of it." }
$older = [double]$want -lt [double]$cur

# ---- Download what's missing, each checked against the list --------------------------------------------
# A package is the listed file when its size is the listed size, the SHA-1 of all but its last 32 bytes is the
# listed SHA-1, and those last 32 bytes are that SHA-1 followed by zeros (the package's own footer).
function Test-Pkg([string]$path, $r) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $fs = [IO.File]::OpenRead($path)
    try {
        if ($fs.Length -ne $r.Size) { return $false }
        $sha = [Security.Cryptography.SHA1]::Create(); $buf = New-Object byte[] (4MB); $left = $r.Size - 32
        while ($left -gt 0) {
            $n = $fs.Read($buf, 0, [int][math]::Min($buf.Length, $left))
            if ($n -le 0) { return $false }
            [void]$sha.TransformBlock($buf, 0, $n, $null, 0); $left -= $n
        }
        [void]$sha.TransformFinalBlock($buf, 0, 0)
        $got = -join ($sha.Hash | ForEach-Object { $_.ToString("x2") })
        $tail = New-Object byte[] 32; [void]$fs.Read($tail, 0, 32)
        $tailHex = -join ($tail | ForEach-Object { $_.ToString("x2") })
        return $got -eq $r.Sha -and $tailHex -eq ($r.Sha + ("0" * 24))
    } finally { $fs.Close() }
}

$need = @(Needed $want)
New-Item -ItemType Directory -Force $Cache | Out-Null
$total = 0; foreach ($r in $need) { if (-not (Have-Pkg $r)) { $total += $r.Size } }
if ($total -gt 0) {
    $free = (Get-PSDrive -Name ($Cache.Substring(0, 1)) -ErrorAction SilentlyContinue).Free
    if ($free -and $free -lt $total * 2.2) {
        Fail ("Not enough free space on {0}: about {1:0.0} GB is needed (the downloads and the installed copy), {2:0.0} GB is free." -f $Cache.Substring(0, 2), ($total * 2.2 / 1GB), ($free / 1GB))
    }
    Say ("== Downloading the game updates from Sony ({0:0} MB)" -f ($total / 1MB)) White
}
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$doneBytes = 0
$files = @()
foreach ($r in $need) {
    $dst = Join-Path $Cache $r.File
    if (Have-Pkg $r) {
        if (Test-Pkg $dst $r) { $files += $dst; continue }
        Say "  $($r.File) isn't the listed file: downloading it again" Yellow
        Remove-Item -LiteralPath $dst -Force
    }
    Say "  update $(Show-Ver $r.Ver) ($([math]::Round($r.Size / 1MB)) MB)"
    $part = "$dst.part"
    $ok = $false
    for ($try = 1; $try -le 4 -and -not $ok; $try++) {
        $have = if (Test-Path -LiteralPath $part) { (Get-Item -LiteralPath $part).Length } else { 0 }
        if ($have -ge $r.Size) { Remove-Item -LiteralPath $part -Force; $have = 0 }
        try {
            $req = [Net.HttpWebRequest]::Create($r.Url)
            $req.UserAgent = "gt5-pc-game-version"; $req.Timeout = 60000; $req.ReadWriteTimeout = 60000
            if ($have -gt 0) { $req.AddRange([int64]$have) }
            $resp = $req.GetResponse()
            if ($have -gt 0 -and [int]$resp.StatusCode -ne 206) { $have = 0 }   # the server sent it all: start over
            $in = $resp.GetResponseStream()
            $out = New-Object IO.FileStream($part, $(if ($have -gt 0) { [IO.FileMode]::Append } else { [IO.FileMode]::Create }), [IO.FileAccess]::Write)
            try {
                $buf = New-Object byte[] (1MB); $got = $have; $tick = [Environment]::TickCount
                while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                    $out.Write($buf, 0, $n); $got += $n
                    if ([Environment]::TickCount - $tick -ge 1000) {
                        $tick = [Environment]::TickCount
                        Write-Host ("  {0:0} MB of {1:0} MB" -f (($doneBytes + $got) / 1MB), ($total / 1MB))
                    }
                }
            } finally { $out.Close(); $in.Close(); $resp.Close() }
            if ((Get-Item -LiteralPath $part).Length -eq $r.Size) { $ok = $true }
        } catch {
            Say "  the download stopped ($($_.Exception.Message)); trying again" Yellow
            Start-Sleep -Seconds ([math]::Min(30, 5 * $try))
        }
    }
    if (-not $ok) { Fail "Downloading update $(Show-Ver $r.Ver) from Sony didn't work. Run it again later: what's downloaded is kept." }
    if (-not (Test-Pkg $part $r)) { Remove-Item -LiteralPath $part -Force; Fail "Update $(Show-Ver $r.Ver) didn't come down as Sony lists it (checksum): nothing was changed. Run it again." }
    Move-Item -LiteralPath $part -Destination $dst -Force
    $doneBytes += $r.Size
    Write-Host ("  {0:0} MB of {1:0} MB" -f ($doneBytes / 1MB), ($total / 1MB))
    $files += $dst
}
if ($need) { Say "  all $($need.Count) update package(s) are here and checked" Green }

# ---- The port's own unlock tool (install_pkg.ps1 unpacks the packages with it too) -------------------------
$tool = Join-Path $PSScriptRoot "ps3_unlock.py"
$py = @((Join-Path $base "tools\python\python.exe"), (Join-Path $base "venv\Scripts\python.exe")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $py) { $c = Get-Command python.exe -ErrorAction SilentlyContinue; if ($c) { $py = $c.Source } }

# ---- The disc's executable back in GT5\input (update_exe.ps1 then switches to the new update's) ----------
$elf = Join-Path $Gt5 "input\EBOOT.elf"
$discElf = Join-Path $Gt5 "input\EBOOT.disc-01.00.elf"
$discSha = @("1535B865EC7DD58E7704B378B8621532ECF844E1", "40126D2D14B0A95B9601BB11E3A6E778186067AC", "49A3662D4B541BB4E0C2E72D2602F89FEB710E71")
function Restore-Disc-Exe {
    if ((Test-Path $elf) -and ($discSha -contains (Get-FileHash $elf -Algorithm SHA1).Hash)) { return $true }
    if (Test-Path $discElf) { Copy-Item -LiteralPath $discElf -Destination $elf -Force; Say "  GT5\input\EBOOT.elf is the disc's again"; return $true }
    # Never kept (an install that started from an update's executable): unlock the disc's own again.
    $bin = Join-Path $Gt5 "vfs\PS3_GAME\USRDIR\EBOOT.BIN"
    if (-not $py -or -not (Test-Path -LiteralPath $tool) -or -not (Test-Path $bin)) { return $false }
    $out = Join-Path $Gt5 "input\EBOOT.disc.tmp.elf"
    & $py -I $tool eboot $bin $out --keys-dir (Join-Path $base "tools\ps3_keys") 2>&1 | Out-Null
    $ok = (Test-Path $out) -and ($discSha -contains (Get-FileHash $out -Algorithm SHA1).Hash)
    if ($ok) { Copy-Item $out $discElf -Force; Move-Item $out $elf -Force; Say "  GT5\input\EBOOT.elf is the disc's again (unlocked from the disc)" }
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    return $ok
}

# ---- Set the installed data aside (put back if anything below fails) --------------------------------------
$aside = "$gameDir.before-version-change"
$elfAside = "$elf.before-version-change"
if (Test-Path $aside) { Remove-Item -LiteralPath $aside -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path $elf) { Copy-Item -LiteralPath $elf -Destination $elfAside -Force }
function Undo([string]$why) {
    Say $why Yellow
    if (Test-Path $aside) {
        if (Test-Path $gameDir) { Remove-Item -LiteralPath $gameDir -Recurse -Force -ErrorAction SilentlyContinue }
        Move-Item -LiteralPath $aside -Destination $gameDir
        Say "  the game's update data is back as it was (version $(Show-Ver $cur))"
    }
    if (Test-Path $elfAside) { Move-Item -LiteralPath $elfAside -Destination $elf -Force }
    exit 2
}
# An installed update's data goes aside in either direction: the install writes a whole new copy, and a change
# that fails part-way must not leave data of one version with the executable of another.
if ($cur -ne "01.00" -and (Test-Path $gameDir)) {
    Say "== Setting update $(Show-Ver $cur)'s data aside until the change has worked" White
    try { Move-Item -LiteralPath $gameDir -Destination $aside -ErrorAction Stop }
    catch { if (Test-Path $elfAside) { Remove-Item $elfAside -Force }; Fail "Couldn't move GT5\hdd0\game\$title aside ($($_.Exception.Message)). Is the game running?" }
}
if ($cur -ne "01.00" -and -not (Restore-Disc-Exe)) { Undo "The disc's executable isn't in GT5\input (EBOOT.disc-01.00.elf) and couldn't be unlocked again." }

if ($want -eq "01.00") {
    if (Test-Path $aside) { Remove-Item -LiteralPath $aside -Recurse -Force -ErrorAction SilentlyContinue }
    if (Test-Path $elfAside) { Remove-Item -LiteralPath $elfAside -Force }
    Say "GT5 is the disc's version 1.00 again." Green
    Say "Next: run setup.bat (or setup.ps1) once. It generates and builds the game for this version (about an hour)." Green
    exit 0
}

# Going on from an installed update: its data goes back in place as a copy (the one set aside stays until the
# change has worked) and the newer updates go on top of it. Going back, or from the disc's 1.00: the updates
# are installed from 1.05 on into an empty folder.
$from = if ($older -or $cur -eq "01.00") { "" } else { $cur }
if ($from) {
    Say "  update $(Show-Ver $from)'s data stays as the start"
    robocopy $aside $gameDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Undo "Copying the update data back failed (robocopy code $LASTEXITCODE)." }
}

# ---- Install them (install_pkg.ps1: unpacked into GT5\hdd0, then the update's executable) -----------------
Say "== Installing $($files.Count) update package(s)" White
$env:GT5_PKG_NOPAUSE = "1"
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "install_pkg.ps1") @files -Gt5 $Gt5
$code = $LASTEXITCODE
$now = Read-Sfo (Join-Path $gameDir "PARAM.SFO")
$now = if ($now -and $now["APP_VER"]) { Norm-Ver ([string]$now["APP_VER"]) } else { "" }
if ($code -ne 0 -or $now -ne $want) { Undo "The game isn't at $(Show-Ver $want) after the install (it reads $(if ($now) { Show-Ver $now } else { 'none' }))." }
if ((Test-Path $elf) -and ($discSha -contains (Get-FileHash $elf -Algorithm SHA1).Hash)) { Undo "The update's executable couldn't be unlocked (see above)." }
if (Test-Path $aside) { Remove-Item -LiteralPath $aside -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path $elfAside) { Remove-Item -LiteralPath $elfAside -Force }
Say "GT5 $title is now at version $(Show-Ver $want)." Green
exit 0
