# Installs GT5 game updates and DLC from their .pkg files, the way RPCS3 does, for the port:
#   powershell -ExecutionPolicy Bypass -File GT5\scripts\install_pkg.ps1 <file.pkg or folder> [more ...]
#                [-Gt5 <GT5 folder>] [-Rpcs3 <rpcs3.exe>]
# (The launcher's Help page has a button for it.)
#
# 1. RPCS3 installs each package into its own dev_hdd0. Updates go in version order (1.05, 1.06 ...
#    2.17), as RPCS3 refuses one whose previous version isn't installed; ones RPCS3 already has are
#    skipped. RPCS3's window opens for each package and this answers its questions.
# 2. The game's folder from RPCS3 (dev_hdd0\game\<title ID>) is copied to GT5\hdd0\game\<title ID>,
#    all but EBOOT.BIN, as the README's Game updates steps do by hand.
# 3. update_exe.ps1 unlocks the newest update's EBOOT.BIN into GT5\input\EBOOT.elf.
# Then setup.bat (or setup.ps1) generates and builds the game for that version once (about an hour).
#
# DLC: a DLC package installs its voucher; the game still needs that licence's key in
# GT5\hdd0\home\00000001\exdata (README, DLC). Nothing here makes or holds keys.
#
# RPCS3 is looked for as update_exe.ps1 does: -Rpcs3, GT5_RPCS3, the installer's tools\rpcs3, PATH, and a
# few usual folders. RPCS3 doesn't run from a temporary folder (it warns and waits).
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

# ---- RPCS3 and its dev_hdd0 -------------------------------------------------------------------------------
$cands = @()
if ($Rpcs3) { $cands += $(if ($Rpcs3 -like "*.exe") { $Rpcs3 } else { Join-Path $Rpcs3 "rpcs3.exe" }) }
if ($env:GT5_RPCS3) { $cands += $(if ($env:GT5_RPCS3 -like "*.exe") { $env:GT5_RPCS3 } else { Join-Path $env:GT5_RPCS3 "rpcs3.exe" }) }
$cands += Join-Path $base "tools\rpcs3\rpcs3.exe"
$onPath = Get-Command rpcs3.exe -ErrorAction SilentlyContinue
if ($onPath) { $cands += $onPath.Source }
$roots = @("$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Documents", "$env:LOCALAPPDATA",
           "$env:ProgramFiles", "${env:ProgramFiles(x86)}")
$roots += @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
    $_.Root; Join-Path $_.Root "Games"; Join-Path $_.Root "Emulators"; Join-Path $_.Root "Downloads" })
foreach ($r in $roots) {
    if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
    foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -Filter "*rpcs3*" -ErrorAction SilentlyContinue)) {
        $cands += Join-Path $d.FullName "rpcs3.exe"
        foreach ($s in @(Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue)) { $cands += Join-Path $s.FullName "rpcs3.exe" }
    }
}
$exe = @($cands | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique) | Select-Object -First 1
if (-not $exe) { Fail "RPCS3 wasn't found. Install it (rpcs3.net), or run this again with -Rpcs3 <path to rpcs3.exe> (or set GT5_RPCS3)." }
if ($exe -match '\\(Temp|tmp)\\') { Say "  note: RPCS3 is in a temporary folder ($exe); it may refuse to start from there." Yellow }
$emu = Split-Path $exe -Parent
$hdd = Join-Path $emu "dev_hdd0"
$vfs = Join-Path $emu "config\vfs.yml"
if (Test-Path $vfs) {
    $line = Get-Content $vfs -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\s*/dev_hdd0/\s*:' } | Select-Object -First 1
    if ($line) {
        $q = ($line -replace '^\s*/dev_hdd0/\s*:\s*', '').Trim().Trim('"').Replace('$(EmulatorDir)', "$emu\")
        if ($q) { $hdd = $q.TrimEnd('/', '\') }
    }
}
Say "RPCS3: $exe" White
Say "  its dev_hdd0: $hdd"

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
    $s = Read-Sfo (Join-Path $hdd "game\$title\PARAM.SFO")
    if ($s -and $s["APP_VER"]) { return ([string]$s["APP_VER"]).Trim() }
    return ""
}
function Count-Files([string]$dir) { return @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue).Count }

# ---- Install each package with RPCS3, answering its questions ---------------------------------------------
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [Windows.Automation.AutomationElement]
$T = [Windows.Automation.TreeScope]
$btnType = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Button)
$txtType = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Text)
function Press($scope, [string]$name) {
    $c = New-Object Windows.Automation.AndCondition($btnType, (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)))
    $b = $scope.FindFirst($T::Descendants, $c)
    if ($b) { try { $b.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); return $true } catch {} }
    return $false
}

$failed = @()
foreach ($k in $pkgs) {
    $name = [IO.Path]::GetFileName($k.File)
    $gameDir = Join-Path $hdd "game\$($k.Title)"
    if ($k.IsUpdate) {
        $have = Installed-Ver $k.Title
        if ($have -and [double]$have -ge [double]$k.Ver) { Say "  update $($k.Ver): RPCS3 already has $have, skipped"; continue }
    }
    Say "Installing $name ..." White
    $before = Count-Files $gameDir
    $p = Start-Process -FilePath $exe -ArgumentList @("--installpkg", "`"$($k.File)`"") -PassThru -WindowStyle Minimized
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
    # RPCS3 0.0.37 asks "Do you want to install this package?" (Yes), shows "Installing package (1/1), please
    # wait...", then "Successfully installed software from package(s)!" with an offer of shortcuts and cache
    # precompiling, which is left unanswered: RPCS3 is closed instead. Its errors ("Package cannot be
    # installed on top of the current data...") are passed on.
    $t0 = Get-Date; $asked = $false; $err = ""; $done = $false; $busy = $false; $stable = 0; $last = -1; $said = @{}
    while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMinutes) {
        if ($p.HasExited) { break }
        $busy = $false
        try {
            foreach ($w in $A::RootElement.FindAll($T::Children, $cond)) {
                $texts = (@($w.FindAll($T::Descendants, $txtType) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }) -join " ")
                if ($texts -match 'Do you want to install') { if (Press $w "Yes") { $asked = $true } }
                elseif ($texts -match '[Ss]uccessfully installed') { $done = $true }
                elseif ($texts -match 'Installing package') { $busy = $true }
                elseif ($texts -match 'cannot be installed|[Ff]ailed|[Ee]rror|not supported|invalid') {
                    $err = ($texts -replace '\s+', ' ').Trim()
                    [void](Press $w "OK")
                }
                elseif ($texts -and -not $said[$texts] -and $texts -match 'RPCS3 should not|temporary') { $said[$texts] = 1; Say "  RPCS3 says: $texts" Yellow }
            }
        } catch {}
        if ($err -or $done) { break }
        # Another RPCS3 build may word its last box differently: the install is also over once the progress box
        # has gone and the files stopped changing.
        if ($asked -and -not $busy) {
            $n = Count-Files $gameDir
            if ($n -gt $before -and $n -eq $last) { $stable++ } else { $stable = 0 }
            $last = $n
            if ($stable -ge 10) { $done = $true; break }
        }
        Start-Sleep -Milliseconds 700
    }
    if (-not $p.HasExited) { [void]$p.CloseMainWindow(); Start-Sleep -Seconds 4; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    if ($done) { Say "  installed$(if ($k.IsUpdate) { ": RPCS3 now has $($k.Title) $($k.Ver)" })" Green }
    else {
        $why = if ($err) { "RPCS3: $err" } elseif (-not $asked) { "RPCS3 didn't offer to install it (is it already open, or showing a first-start window?)" } else { "it didn't finish in $TimeoutMinutes minutes" }
        Say "  not installed: $why" Yellow
        $failed += $name
        if ($k.IsUpdate) { Say "  The updates after it need it, so stopping here." Yellow; break }
    }
}

# ---- Copy what RPCS3 installed into GT5\hdd0, and unlock the update's executable ----------------------------
foreach ($t in $titles) {
    $src = Join-Path $hdd "game\$t"
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $Gt5 "hdd0\game\$t"
    Say "Copying RPCS3's $t folder to GT5\hdd0\game\$t (all but EBOOT.BIN) ..." White
    New-Item -ItemType Directory -Force $dst | Out-Null
    robocopy $src $dst /E /XF EBOOT.BIN /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "  copying failed (robocopy code $LASTEXITCODE)." }
    $v = (Read-Sfo (Join-Path $dst "PARAM.SFO"))
    Say "  done$(if ($v -and $v['APP_VER']) { ": GT5\hdd0 now holds $t version $($v['APP_VER'])" })" Green
}
$env:GT5_RPCS3 = $exe
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "update_exe.ps1") -Gt5 $Gt5
$code = $LASTEXITCODE

Say ""
if ($failed) { Say "Not installed: $($failed -join ', ')" Yellow }
if ($code -eq 2) { Say "The update's data is in place, but its executable couldn't be unlocked (see above)." Yellow }
elseif ($pkgs | Where-Object IsUpdate) { Say "Next: run setup.bat (or setup.ps1) once. It generates and builds the game for this version (about an hour)." Green }
if ($pkgs | Where-Object { -not $_.IsUpdate }) { Say "DLC: the vouchers are in place; each still needs its licence key in GT5\hdd0\home\00000001\exdata (README, DLC)." }
if ($env:GT5_PKG_NOPAUSE -ne "1") { Read-Host "Press Enter to close" | Out-Null }
exit $(if ($failed) { 1 } else { 0 })
