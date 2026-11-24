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
# few usual folders, then in the packages' own folder and the folders above it (a PS3 folder that holds
# both RPCS3 and the downloaded updates). With none found it fetches RPCS3 into tools\rpcs3, as the
# installer does (installs since v0.1.105 no longer bring it). RPCS3 doesn't run from a temporary folder
# (it warns and waits). For testing: GT5_PKG_FIND_ONLY=1 stops once RPCS3 is found or fetched;
# GT5_PKG_NO_SEARCH=1 skips the search of the PC (only -Rpcs3, GT5_RPCS3 and tools\rpcs3 count).
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
if ($env:GT5_PKG_NO_SEARCH -ne "1") {
    $onPath = Get-Command rpcs3.exe -ErrorAction SilentlyContinue
    if ($onPath) { $cands += $onPath.Source }
    $roots = @("$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Documents", "$env:LOCALAPPDATA",
               "$env:ProgramFiles", "${env:ProgramFiles(x86)}")
    $roots += @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
        $_.Root; Join-Path $_.Root "Games"; Join-Path $_.Root "Emulators"; Join-Path $_.Root "Downloads" })
    # The packages' folder and up to four above it: RPCS3 itself there (F:\PS3\rpcs3.exe beside
    # F:\PS3\updates\...), or a folder with "rpcs3" in its name.
    foreach ($k in $pkgs) {
        $d = Split-Path $k.File -Parent
        for ($up = 0; $d -and $up -lt 5; $up++) {
            $cands += Join-Path $d "rpcs3.exe"
            $roots += $d
            $d = Split-Path $d -Parent
        }
    }
    foreach ($r in ($roots | Select-Object -Unique)) {
        if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
        foreach ($d in @(Get-ChildItem -LiteralPath $r -Directory -Filter "*rpcs3*" -ErrorAction SilentlyContinue)) {
            $cands += Join-Path $d.FullName "rpcs3.exe"
            foreach ($s in @(Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue)) { $cands += Join-Path $s.FullName "rpcs3.exe" }
        }
    }
}
$exe = @($cands | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique) | Select-Object -First 1

# None on this PC: RPCS3's newest Windows build from its GitHub releases into tools\rpcs3, checked against
# the SHA-256 published with it, unpacked into rpcs3.part and renamed when complete (as GT5-Installer.bat's
# Get-Rpcs3). Windows 11's tar reads the .7z; Windows 10's can't, so 7-Zip, or 7-Zip's own small extractor.
function Get-OwnRpcs3 {
    $dir = Join-Path $base "tools\rpcs3"
    $part = "$dir.part"
    Say "RPCS3 wasn't found on this PC: getting it into tools\rpcs3 (about 40 MB, once)..." White
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $rel = Invoke-RestMethod "https://api.github.com/repos/RPCS3/rpcs3-binaries-win/releases/latest" -Headers @{ "User-Agent" = "gt5-pc-install-pkg" } -TimeoutSec 60
    } catch { Say "  couldn't reach GitHub ($($_.Exception.Message))" Yellow; return $null }
    $asset = $rel.assets | Where-Object { $_.name -like "*win64*.7z" } | Select-Object -First 1
    if (-not $asset) { Say "  RPCS3's Windows download wasn't in its newest release" Yellow; return $null }
    $sum = $rel.assets | Where-Object { $_.name -eq "$($asset.name).sha256" } | Select-Object -First 1
    Remove-Item -LiteralPath $part -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $part | Out-Null
    $arc = Join-Path $part $asset.name
    $old = $ProgressPreference; $ProgressPreference = "SilentlyContinue"
    try { Invoke-WebRequest $asset.browser_download_url -OutFile $arc -UseBasicParsing -Headers @{ "User-Agent" = "gt5-pc-install-pkg" } -TimeoutSec 600 }
    catch { Say "  the download failed ($($_.Exception.Message))" Yellow; Remove-Item -LiteralPath $part -Recurse -Force -ErrorAction SilentlyContinue; return $null }
    finally { $ProgressPreference = $old }
    if ($sum) {
        $want = ""
        # As text: Windows PowerShell's Invoke-WebRequest gives this file (application/octet-stream) as bytes.
        try { $wc = New-Object Net.WebClient; $wc.Headers.Add("User-Agent", "gt5-pc-install-pkg")
              $want = ($wc.DownloadString($sum.browser_download_url).Trim() -split '\s+')[0] } catch {}
        if ($want -and (Get-FileHash -LiteralPath $arc -Algorithm SHA256).Hash -ne $want.ToUpper()) {
            Say "  the download was damaged (checksum); nothing kept" Yellow
            Remove-Item -LiteralPath $part -Recurse -Force -ErrorAction SilentlyContinue; return $null
        }
    }
    & tar.exe -xf $arc -C $part 2>$null
    if (-not (Test-Path -LiteralPath (Join-Path $part "rpcs3.exe"))) {
        $sz = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $sz) {
            $sz = Join-Path $base "tools\7zr.exe"
            if (-not (Test-Path -LiteralPath $sz)) {
                try { Invoke-WebRequest "https://www.7-zip.org/a/7zr.exe" -OutFile $sz -UseBasicParsing -TimeoutSec 120 } catch {}
            }
        }
        if (Test-Path -LiteralPath $sz) { & $sz x -y "-o$part" $arc | Out-Null }
    }
    Remove-Item -LiteralPath $arc -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath (Join-Path $part "rpcs3.exe"))) {
        Say "  unpacking it failed (7-Zip from https://www.7-zip.org can do it)" Yellow
        Remove-Item -LiteralPath $part -Recurse -Force -ErrorAction SilentlyContinue; return $null
    }
    # No welcome box, no update check, no pop-ups: it's only used from here.
    New-Item -ItemType Directory -Force (Join-Path $part "GuiConfigs") | Out-Null
    [IO.File]::WriteAllText((Join-Path $part "GuiConfigs\CurrentSettings.ini"),
        "[main_window]`r`ninfoBoxEnabledWelcome=false`r`ninfoBoxEnabledInstallPUP=false`r`ninfoBoxEnabledInstallPKG=false`r`n[Meta]`r`ncheckUpdateStart=false`r`n")
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $part -Destination $dir
    return (Join-Path $dir "rpcs3.exe")
}
if (-not $exe) { $exe = Get-OwnRpcs3 }
if (-not $exe) { Fail "RPCS3 wasn't found and couldn't be downloaded (see above). Install it from rpcs3.net, or run this again with -Rpcs3 <path to rpcs3.exe>." }
if ($env:GT5_PKG_FIND_ONLY -eq "1") { Say "RPCS3: $exe" White; exit 0 }
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

# ---- Updates go in order: a gap is said plainly before RPCS3 starts ---------------------------------------
# RPCS3 refuses an update whose predecessor it doesn't have ("Update with version 02.01 is for version 02.00,
# but you don't have any data installed": a player picked 2.01 to 2.11 without 1.05 to 2.00, 23 Nov 2026).
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
            $from = if ($have) { "RPCS3 has $t $have, so" } else { "Nothing of $t is installed yet, so" }
            Say "Updates install only in order, one after the other. $from these are missing before $($given[-1]): $($missing -join ', ')." Yellow
            Say "Pick those packages too (all of them from $($missing[0]) up to $($given[-1])), or use the launcher's Help > Game version and DLC, which downloads every update it needs from Sony." Yellow
            Fail "Nothing was installed."
        }
    }
}

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
# Newer RPCS3 puts its offers (precompile caches, desktop / Start menu / Steam shortcuts) in the question
# itself, some of them ticked: none is wanted here, so each is cleared before the question is answered.
$chkType = New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::CheckBox)
function Untick($scope) {
    foreach ($c in $scope.FindAll($T::Descendants, $chkType)) {
        if ($c.Current.Name -notmatch '[Ss]hortcut|[Pp]recompile') { continue }
        try {
            $tp = $c.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
            for ($i = 0; $i -lt 2 -and $tp.Current.ToggleState -ne [Windows.Automation.ToggleState]::Off; $i++) { $tp.Toggle() }
        } catch {}
    }
}

$failed = @()
$updOk = 0   # updates RPCS3 has now (installed in this run, or already there)
foreach ($k in $pkgs) {
    $name = [IO.Path]::GetFileName($k.File)
    $gameDir = Join-Path $hdd "game\$($k.Title)"
    if ($k.IsUpdate) {
        $have = Installed-Ver $k.Title
        if ($have -and [double]$have -ge [double]$k.Ver) { Say "  update $($k.Ver): RPCS3 already has $have, skipped"; $updOk++; continue }
    }
    Say "Installing $name ..." White
    $before = Count-Files $gameDir
    $p = Start-Process -FilePath $exe -ArgumentList @("--installpkg", "`"$($k.File)`"") -PassThru -WindowStyle Minimized
    $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
    # RPCS3 0.0.37 asks "Do you want to install this package?" (Yes), shows "Installing package (1/1), please
    # wait...", then "Successfully installed software from package(s)!" with an offer of shortcuts and cache
    # precompiling, which is left unanswered: RPCS3 is closed instead. RPCS3 0.0.43 asks the same question in
    # a "PKG Installation" window whose button is Install, with those offers as tick boxes in it (cleared
    # first). Its errors ("Package cannot be installed on top of the current data...") are passed on.
    $t0 = Get-Date; $asked = $false; $err = ""; $done = $false; $busy = $false; $stable = 0; $last = -1; $said = @{}
    while (((Get-Date) - $t0).TotalMinutes -lt $TimeoutMinutes) {
        if ($p.HasExited) { break }
        $busy = $false
        try {
            foreach ($w in $A::RootElement.FindAll($T::Children, $cond)) {
                $texts = (@($w.FindAll($T::Descendants, $txtType) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }) -join " ")
                if ($texts -match 'Do you want to install') { Untick $w; if ((Press $w "Yes") -or (Press $w "Install")) { $asked = $true } }
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
    if ($done) { Say "  installed$(if ($k.IsUpdate) { ": RPCS3 now has $($k.Title) $($k.Ver)" })" Green; if ($k.IsUpdate) { $updOk++ } }
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
# Only when RPCS3 has an update now: with none installed nothing changed, and there is nothing to build.
if ($code -eq 2) { Say "The update's data is in place, but its executable couldn't be unlocked (see above)." Yellow }
elseif ($updOk -gt 0) { Say "Next: run setup.bat (or setup.ps1) once. It generates and builds the game for this version (about an hour)." Green }
if ($pkgs | Where-Object { -not $_.IsUpdate }) { Say "DLC: the vouchers are in place; each still needs its licence key in GT5\hdd0\home\00000001\exdata (README, DLC)." }
if ($env:GT5_PKG_NOPAUSE -ne "1") { Read-Host "Press Enter to close" | Out-Null }
exit $(if ($failed) { 1 } else { 0 })
