# Installs (or removes) a GT5 game mod that comes as a zip of PDIPFS files, such as the GT5 Master Mod:
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\install_mod.ps1 <mod.zip> [-Gt5 <GT5 folder>]
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\install_mod.ps1 -Remove   [-Gt5 <GT5 folder>]
# (The launcher's Help page has a button for it.)
#
# Such a mod is a PDIPFS folder whose files go over the game's own update data, here
# GT5\hdd0\game\<title ID>\USRDIR\PDIPFS (on a PS3 or in RPCS3: dev_hdd0\game\<title ID>\USRDIR\PDIPFS).
# This copies them there straight from the zip. Nothing in the zip is run, and nothing else in it is used.
#
# - The mod has to fit the game: the Master Mod needs game update 2.11 exactly, and its zip for the same
#   release (Europe BCES00569, North America BCUS98114 ...), as the game's files are locked per release.
#   A zip that brings the game's original files (the Master Mod's PDIPFS_ORIGINAL folder) is checked against
#   the installed game before anything is copied; one that doesn't fit is refused.
# - Every game file the mod replaces is kept in USRDIR\PDIPFS_before_mod, and USRDIR\mod_installed.txt lists
#   what was added and replaced, so -Remove puts the game back exactly. Installing another mod (or a newer
#   version of the same one) into a game first removes the one that is there.
# - A save made with a mod may not load without it (cars and tracks the game doesn't have).
param([Parameter(Position = 0)][string]$Zip = "", [switch]$Remove, [string]$Gt5 = "")
$ErrorActionPreference = "Stop"
if (-not $Gt5) { $Gt5 = Split-Path $PSScriptRoot -Parent }

function Say([string]$s, [string]$c = "Gray") { Write-Host $s -ForegroundColor $c }
function Fail([string]$s) { Say $s Yellow; exit 2 }
function Same([byte[]]$a, [byte[]]$b) {
    if ($null -eq $a -or $null -eq $b -or $a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
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
        if ($fmt -ne 0x0404) { $r[$k] = [Text.Encoding]::UTF8.GetString($b, $dataT + $do, $len).TrimEnd([char]0) }
    }
    return $r
}
function Game-Ver([string]$dir) {
    $s = Read-Sfo (Join-Path $dir "PARAM.SFO")
    if ($s -and $s["APP_VER"]) { return ([string]$s["APP_VER"]).Trim().TrimStart("0") }
    return ""
}

# ---- The game folders in GT5\hdd0 (one per release that was installed or updated here) ----------------------
$gameRoot = Join-Path $Gt5 "hdd0\game"
$games = @()
if (Test-Path -LiteralPath $gameRoot) {
    $games = @(Get-ChildItem -LiteralPath $gameRoot -Directory | Where-Object {
        $_.Name -match '^[A-Z]{4}\d{5}$' -and (Test-Path -LiteralPath (Join-Path $_.FullName "USRDIR\PDIPFS")) })
}

# Puts back what a mod replaced and deletes what it added, from its list. Returns the number of files handled.
function Remove-Mod([string]$usr) {
    $list = Join-Path $usr "mod_installed.txt"
    $keep = Join-Path $usr "PDIPFS_before_mod"
    $pd = Join-Path $usr "PDIPFS"
    $n = 0
    foreach ($l in [IO.File]::ReadAllLines($list)) {
        if ($l.Length -lt 3 -or ($l[0] -ne '+' -and $l[0] -ne '*')) { continue }
        $rel = $l.Substring(2).Trim()
        if (-not $rel -or $rel.Contains("..") -or [IO.Path]::IsPathRooted($rel)) { continue }
        $dst = Join-Path $pd $rel
        if ($l[0] -eq '*') {
            $old = Join-Path $keep $rel
            if (-not (Test-Path -LiteralPath $old)) { Fail "The game's own copy of PDIPFS\$rel is missing from PDIPFS_before_mod: nothing more was changed. Install the game update again to put the game back." }
            Copy-Item -LiteralPath $old -Destination $dst -Force
        } elseif (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }
        $n++
    }
    Remove-Item -LiteralPath $keep -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $list -Force
    return $n
}

if ($Remove) {
    $done = 0
    foreach ($g in $games) {
        $usr = Join-Path $g.FullName "USRDIR"
        if (-not (Test-Path -LiteralPath (Join-Path $usr "mod_installed.txt"))) { continue }
        $name = ([IO.File]::ReadAllLines((Join-Path $usr "mod_installed.txt")) | Where-Object { $_ -like "mod: *" } | Select-Object -First 1) -replace '^mod: ', ''
        $n = Remove-Mod $usr
        Say "Removed the mod from $($g.Name): $name ($n files; the game's own files are back)." Green
        $done++
    }
    if (-not $done) { Say "No mod is installed here." White }
    exit 0
}

if (-not $Zip) { Fail "No mod given. Usage: install_mod.ps1 <mod.zip>   (or -Remove)" }
$Zip = $Zip.Trim('"')
if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { Fail "Not found: $Zip" }
if (-not $games) { Fail "This install has no game update data yet (GT5\hdd0\game\<title ID>\USRDIR\PDIPFS). Install the game update the mod needs first: the launcher's Help page, Install a game update." }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
try { $z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path) } catch { Fail "That file isn't a zip that can be read." }
try {
    # The mod's files: every entry under the PDIPFS folder nearest the top of the zip.
    $files = @($z.Entries | Where-Object { $_.Name })      # folders have an empty Name
    $root = $null
    foreach ($e in $files) {
        $parts = $e.FullName.Replace('\', '/').Split('/')
        $i = [Array]::IndexOf($parts, "PDIPFS")
        if ($i -ge 0 -and $i -lt $parts.Length - 1 -and ($null -eq $root -or $i + 1 -lt $root.Count)) { $root = @($parts[0..$i]) }
    }
    if ($null -eq $root) { Fail "That zip has no PDIPFS folder in it, so it isn't a mod this can install." }
    $prefix = ($root -join "/") + "/"
    $origPrefix = $prefix.Substring(0, $prefix.Length - "PDIPFS/".Length) + "PDIPFS_ORIGINAL/"
    $mod = @{}; $orig = @{}
    foreach ($e in $files) {
        $n = $e.FullName.Replace('\', '/')
        foreach ($set in @(@($prefix, $mod), @($origPrefix, $orig))) {
            if (-not $n.StartsWith($set[0])) { continue }
            $rel = $n.Substring($set[0].Length)
            if ($rel -notmatch '^[A-Za-z0-9_.\-]+(/[A-Za-z0-9_.\-]+)*$' -or $rel.Contains("..")) { Fail "That zip has a file name this won't write ($n): nothing was changed." }
            $set[1][$rel] = $e
        }
    }
    function Bytes($entry) {
        $s = $entry.Open(); $m = New-Object IO.MemoryStream
        try { $s.CopyTo($m) } finally { $s.Close() }
        return $m.ToArray()
    }
    $modName = [IO.Path]::GetFileNameWithoutExtension($Zip)
    Say "Mod: $modName ($($mod.Count) files)" White

    # Which game it is for.
    $target = $null
    if ($orig.Count) {
        # The zip brings the game files it was made from: the game here must have exactly those. Where a mod
        # is installed already, the game's own copy of a file it replaced is the one in PDIPFS_before_mod.
        foreach ($g in $games) {
            $fits = $true
            foreach ($rel in $orig.Keys) {
                $have = Join-Path $g.FullName ("USRDIR\PDIPFS_before_mod\" + $rel.Replace('/', '\'))
                if (-not (Test-Path -LiteralPath $have)) { $have = Join-Path $g.FullName ("USRDIR\PDIPFS\" + $rel.Replace('/', '\')) }
                if (-not (Test-Path -LiteralPath $have) -or -not (Same ([IO.File]::ReadAllBytes($have)) (Bytes $orig[$rel]))) { $fits = $false; break }
            }
            if ($fits) { $target = $g; break }
        }
        if (-not $target) {
            $here = ($games | ForEach-Object { "$($_.Name) $(Game-Ver $_.FullName)".Trim() }) -join ", "
            Fail ("This mod doesn't fit the game that is installed here ($here). It needs the game update it was made for, and its zip for the same release of the game " +
                  "(the Master Mod: update 2.11 exactly, and the zip named for your game's ID). Nothing was changed.")
        }
    } elseif ($games.Count -eq 1) {
        $target = $games[0]
        Say "  note: this zip doesn't bring the game's original files, so it couldn't be checked that it fits this game version" Yellow
    } else {
        Fail "More than one game is installed here ($(($games | ForEach-Object Name) -join ', ')) and this zip doesn't say which it is for. Nothing was changed."
    }

    $usr = Join-Path $target.FullName "USRDIR"
    $pd = Join-Path $usr "PDIPFS"
    $keep = Join-Path $usr "PDIPFS_before_mod"
    Say "Game: $($target.Name) $(Game-Ver $target.FullName)" White
    # A mod already in this game is taken out first, so what gets kept below is the game's own files.
    if (Test-Path -LiteralPath (Join-Path $usr "mod_installed.txt")) {
        $n = Remove-Mod $usr
        Say "  took out the mod that was installed first ($n files)"
    }
    $lines = New-Object Collections.Generic.List[string]
    $lines.Add("# Written by install_mod.ps1: what the mod added (+) and replaced (*) in PDIPFS. -Remove reads this.")
    $lines.Add("mod: $modName")
    $lines.Add("installed: " + (Get-Date).ToString("yyyy-MM-dd HH:mm"))
    # The list is written before the first file, and again complete at the end: a copy that is cut short can
    # still be taken out.
    $list = Join-Path $usr "mod_installed.txt"
    $added = 0; $replaced = 0
    foreach ($rel in ($mod.Keys | Sort-Object)) {
        $dst = Join-Path $pd $rel.Replace('/', '\')
        if (Test-Path -LiteralPath $dst) {
            $old = Join-Path $keep $rel.Replace('/', '\')
            New-Item -ItemType Directory -Force (Split-Path $old -Parent) | Out-Null
            Copy-Item -LiteralPath $dst -Destination $old -Force
            $lines.Add("* $rel"); $replaced++
        } else { $lines.Add("+ $rel"); $added++ }
    }
    [IO.File]::WriteAllLines($list, $lines)
    foreach ($rel in ($mod.Keys | Sort-Object)) {
        $dst = Join-Path $pd $rel.Replace('/', '\')
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($mod[$rel], $dst, $true)
    }
    Say "Installed the mod: $added files added, $replaced replaced (kept in PDIPFS_before_mod)." Green
    Say "The first start after this can take a while: the game rebuilds its lists."
    Say "To take it out again: install_mod.ps1 -Remove (the launcher's Help page has it too)."
} finally { $z.Dispose() }
exit 0
