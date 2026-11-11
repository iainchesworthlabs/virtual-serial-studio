# Copyright (c) 2026 Virtual Serial Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a 32-bit serial program, and then a hung one, against the installed vsscom driver.
# Run inside a guest, as an administrator, after install-driver.ps1 (add -Helper and
# -Flavour umdf for the user-mode driver), with vss_com_client_x86.exe next to this script
# or in -ClientDir. vss_com_client_x64.exe (or _arm64) is optional and is what the 32-bit
# results are compared with.
#
#   test-wow64.ps1 [-ClientDir <dir>] [-ResultPath <json>] [-Flavour kmdf|umdf] [-RevokeCode <n>]
#
# The driver is x64 and ARM64 only. A "32-bit guest" here is a 64-bit Windows guest that runs
# the 32-bit program under WOW64, which is what a 32-bit program on a 64-bit machine does. A
# 32-bit Windows cannot load an x64 driver, and there is no x86 driver build.
#
# Tests (W are about the API, T are the hung-program tests of gap G19 in the gap survey):
#   W1 probe          COM1 to COM16, with and without \\.\, free and while another program holds the target
#   W2 exercise x86   the calls of an old program, against a port whose hub end echoes
#   W3 exercise 64    the same with the 64-bit build
#   W4 same results   the two builds agree, step by step
#   W5 line errors    the client asks for the error character and abort on error; the hub raises parity, break and framing errors
#   T1 kill           a pending read, a pending event wait and a write stalled on a full queue; kill the program
#   T2 suspend        the same, with the program suspended: what a second open gets, then the kill
#   T3 sync read      a synchronous read with no timeout; kill the program
#   T4 close outside  close the handle in the suspended program from outside, cancel its requests
#   T5 revoke         the driver's revoke request on the suspended program (skipped without the request)
#   T6 port removed   remove the port while a handle is held, make it again: which slot and COM name
#   T7 helper restart restart the helper service with a program attached (user-mode driver only)
#
# A status is pass, fail, skip or unexpected. unexpected means the driver did something the
# design did not predict, but nothing the test was built to prove has failed; the detail says
# what happened. They are results to read, not to hide. Exit code 1 when a test failed.
#
# T5 needs a revoke request in the driver: IOCTL_VSSCOM_HUB_REVOKE in public.h (an enumerator
# or a #define), sent to the hub end with no input and no output. The script finds its control code in public.h (next to
# this script, or in ..\vsscom), or takes it from -RevokeCode.

[CmdletBinding()]
param(
    [string] $ClientDir = $PSScriptRoot,
    [string] $ResultPath,
    [ValidateSet('kmdf', 'umdf')] [string] $Flavour,
    [string] $RevokeCode,
    [string[]] $Only
)

$ErrorActionPreference = 'Stop'
if ($Flavour) { $env:VSS_DRIVER = $Flavour }
. (Join-Path $PSScriptRoot 'common.ps1')
. (Join-Path $PSScriptRoot 'hub-io.ps1')

$WorkDir = Join-Path ([IO.Path]::GetTempPath()) 'vss-wow64'
$OutDir = if ($ResultPath) { Split-Path -Parent ([IO.Path]::GetFullPath($ResultPath)) } else { $WorkDir }
New-Item -ItemType Directory -Force $WorkDir, $OutDir | Out-Null

$nativeArch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$nativeName = if ($nativeArch -eq 'ARM64') { 'arm64' } else { 'x64' }
$Exe86 = Join-Path $ClientDir 'vss_com_client_x86.exe'
$ExeNative = Join-Path $ClientDir "vss_com_client_$nativeName.exe"
if (-not (Test-Path $Exe86)) { throw "$Exe86 is not there" }
$HaveNative = Test-Path $ExeNative

$ERROR_ACCESS_DENIED = 5

# ---- Revoke control code ----------------------------------------------------------------
# CTL_CODE(device, function, method, access) from public.h, if the driver has the request.
function Get-RevokeControlCode {
    if ($RevokeCode) { return [uint32]$RevokeCode }
    foreach ($candidate in (Join-Path $PSScriptRoot 'public.h'), (Join-Path $PSScriptRoot '..\vsscom\public.h')) {
        if (-not (Test-Path $candidate)) { continue }
        $text = (Get-Content -Raw $candidate) -replace '\\\r?\n', ' '
        if ($text -match 'IOCTL_VSSCOM_HUB_REVOKE\s*=?\s*CTL_CODE\(\s*FILE_DEVICE_VSSCOM\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*METHOD_(\w+)\s*,\s*FILE_(\w+)\s*\)') {
            $function = [uint32]$Matches[1]
            $method = @{ BUFFERED = 0; IN_DIRECT = 1; OUT_DIRECT = 2; NEITHER = 3 }[$Matches[2]]
            $access = @{ ANY_ACCESS = 0; READ_ACCESS = 1; WRITE_ACCESS = 2 }[$Matches[3]]
            return [uint32](([uint32]0x8000 -shl 16) -bor ([uint32]$access -shl 14) -bor ($function -shl 2) -bor [uint32]$method)
        }
    }
    $null
}
$RevokeControl = Get-RevokeControlCode

# ---- Test plumbing ----------------------------------------------------------------------
$tests = @()
$HelperRunning = [bool]([IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$HelperPipeName")

# A test body returns its detail. It throws 'SKIP: ...' to skip and 'UNEXPECTED: ...' to report
# a result the design did not predict; any other exception is a failure.
function Add-Test([string] $Id, [string] $Name, [scriptblock] $Body) {
    if ($Only -and $Id -notin $Only) { return }
    try {
        $detail = & $Body
        $script:tests += New-TestRecord $Id $Name 'pass' ($detail -join '; ')
    } catch {
        $message = $_.Exception.Message
        if ($message -like 'SKIP: *') { $script:tests += New-TestRecord $Id $Name 'skip' $message.Substring(6) }
        elseif ($message -like 'UNEXPECTED: *') { $script:tests += New-TestRecord $Id $Name 'unexpected' $message.Substring(12) }
        else { $script:tests += New-TestRecord $Id $Name 'fail' $message }
    }
}
function Assert([bool] $Condition, [string] $Message) { if (-not $Condition) { throw $Message } }

function Open-Hub([string] $Path) {
    $e = 0
    $hub = [WowHub]::Open($Path, [ref]$e)
    if (-not $hub) { throw "opening the hub end failed with error $e" }
    $hub
}

# What asks for ports: the kernel-mode driver's control device, or the user-mode driver's
# helper service. $null when neither is there.
$VssCtlPath = Get-VssControlPath
function Open-Ctl {
    $e = 0
    if ($Flavour -eq 'umdf') {
        if (-not $HelperRunning) { return $null }
        $helper = [WowHelper]::Open($HelperPipeName, [ref]$e)
        if (-not $helper) { throw "opening the helper's pipe failed with error $e" }
        return $helper
    }
    if (-not $VssCtlPath) { return $null }
    $ctl = [WowCtl]::Open($VssCtlPath, [ref]$e)
    if (-not $ctl) { throw "opening the control device failed with error $e" }
    $ctl
}

# A port for a test: made on demand when something can make one (Ctl is the handle that owns
# it), otherwise the first port made by hand (Ctl is $null and the port stays).
function New-TestPort([switch] $NeedsDemand) {
    $ctl = Open-Ctl
    if (-not $ctl) {
        if ($NeedsDemand) { throw 'SKIP: nothing makes ports on demand (control device, or the helper service for the user-mode driver)' }
        $name = Get-VssComPort
        if (-not $name) { throw 'there is no port: run install-driver.ps1' }
        $hubs = Get-VssHubPaths
        return [pscustomobject]@{ Slot = $null; Port = $name; Path = $hubs[$name]; Ctl = $null }
    }
    $slot = [uint32]0
    $e = $ctl.Create(0, [ref]$slot)
    if ($e -ne 0) { $ctl.Dispose(); throw "CREATE_PORT failed with error $e" }
    $found = Wait-VssDemandPort $slot 60
    if (-not $found) { $ctl.Dispose(); throw "the port in slot $slot did not appear within 60 seconds" }
    [pscustomobject]@{ Slot = $slot; Port = $found.Port; Path = $found.Path; Ctl = $ctl }
}

# Ends a port from New-TestPort, once nothing holds it, and waits until it has gone.
function Remove-TestPort($Port) {
    if ($Port.Ctl) {
        $Port.Ctl.Dispose()
        if ($null -ne $Port.Slot) { Wait-VssDemandPort $Port.Slot 60 -Gone | Out-Null }
    }
}

function Stop-Clients {
    Get-Process -Name 'vss_com_client_*' -ErrorAction SilentlyContinue | ForEach-Object {
        try { $_.Kill(); $_.WaitForExit(5000) | Out-Null } catch { }
    }
}

# Runs a client mode that ends by itself and returns the code, the parsed JSON and the text.
function Invoke-Client([string] $Exe, [string[]] $Arguments, [string] $OutName) {
    $text = (& $Exe @Arguments 2>&1 | Out-String)
    $code = $LASTEXITCODE
    if ($OutName) { Set-Content -Path (Join-Path $OutDir $OutName) -Value $text -Encoding UTF8 }
    $json = $null
    try { $json = $text | ConvertFrom-Json } catch { }
    [pscustomobject]@{ Code = $code; Json = $json; Text = $text }
}

# Starts the client in hold mode and waits for its ready file. Returns the process and what the
# program says it has pending.
$holdCounter = 0
function Start-Hold([string] $Port, [string] $Scenario) {
    $script:holdCounter++
    $ready = Join-Path $WorkDir "ready-$script:holdCounter.json"
    $log = Join-Path $WorkDir "hold-$script:holdCounter.log"
    Remove-Item $ready, $log -ErrorAction SilentlyContinue
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe86
    $psi.Arguments = "hold --port $Port --ready `"$ready`" --log `"$log`" --scenario $Scenario"
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($psi)
    $until = (Get-Date).AddSeconds(30)
    while (-not (Test-Path $ready) -and -not $process.HasExited -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $ready)) {
        try { $process.Kill() } catch { }
        throw "the 32-bit program did not report ready (exited: $($process.HasExited))"
    }
    $info = Get-Content -Raw $ready | ConvertFrom-Json
    if (-not $info.ready) { throw "the 32-bit program could not open $Port (error $($info.open_error) $($info.open_error_name))" }
    [pscustomobject]@{ Process = $process; Pid = $process.Id; Handle = [int64]$info.handle; Info = $info; Log = $log }
}

function Stop-Hold($Hold) {
    if (-not $Hold) { return }
    try { if (-not $Hold.Process.HasExited) { $Hold.Process.Kill() } } catch { }
    try { $Hold.Process.WaitForExit(10000) | Out-Null } catch { }
}

function Get-HoldLog($Hold) {
    if (Test-Path $Hold.Log) { (Get-Content $Hold.Log) -join ' | ' } else { '(no log)' }
}

function Describe-Pending($Hold) {
    "pending: read=$($Hold.Info.read_pending) event wait=$($Hold.Info.wait_pending) write=$($Hold.Info.write_pending)"
}

# Kills the program and measures how long AppOpen takes to clear. Returns a record.
function Measure-Kill($Hold, $Hub) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $Hold.Process.Kill()
    $cleared = $Hub.WaitAppOpen(0, 5000)
    $total = $watch.ElapsedMilliseconds
    $exited = $Hold.Process.WaitForExit(5000)
    [pscustomobject]@{ ClearedMs = $cleared; TotalMs = $total; Exited = $exited; ExitMs = $watch.ElapsedMilliseconds }
}

function Open-Again($PortName) {
    $r = Invoke-Client $Exe86 @('open', '--port', $PortName)
    if (-not $r.Json) { throw "the open probe answered: $($r.Text)" }
    $r.Json
}

# ---- W: the API ---------------------------------------------------------------------------
Stop-Clients
$wctx = $null
function Get-WowContext {
    if ($script:wctx) { return $script:wctx }
    $p = New-TestPort
    $hub = Open-Hub $p.Path
    $echo = New-Object WowEcho $hub
    $echo.Start()
    $script:wctx = [pscustomobject]@{ Port = $p; Hub = $hub; Echo = $echo }
    $script:wctx
}

function Compare-Probe($A, $B) {
    $diffs = @()
    foreach ($row in $A.ports) {
        $other = $B.ports | Where-Object { $_.n -eq $row.n }
        foreach ($form in 'plain', 'prefixed') {
            if ($row.$form.ok -ne $other.$form.ok -or $row.$form.error -ne $other.$form.error) {
                $diffs += "COM$($row.n) $form : $($row.$form.error_name) / $($other.$form.error_name)"
            }
        }
    }
    $diffs
}

Add-Test 'W1' 'a 32-bit program probes COM1 to COM16 with and without the \\.\ prefix' {
    $c = Get-WowContext
    $n = [int]($c.Port.Port -replace '\D', '')
    $free = Invoke-Client $Exe86 @('probe', '--first', '1', '--last', '16') 'probe-x86-free.json'
    Assert ($free.Json) "the probe answered: $($free.Text)"
    Assert ($free.Json.wow64) 'the probe did not run under WOW64'
    $row = $free.Json.ports | Where-Object { $_.n -eq $n }
    Assert ($row.prefixed.ok) "\\.\COM$n did not open: $($row.prefixed.error_name)"
    $detail = @()
    $detail += "runs as $($free.Json.arch) under WOW64 on $($free.Json.native_machine), Windows $($free.Json.windows)"
    $opened = @($free.Json.ports | Where-Object { $_.prefixed.ok } | ForEach-Object { "COM$($_.n)" })
    $detail += "opened with the prefix: $($opened -join ' ')"
    $plain = @($free.Json.ports | Where-Object { $_.plain.ok } | ForEach-Object { "COM$($_.n)" })
    $detail += "opened without it: $($plain -join ' ')"
    $notes = @()
    if ($n -le 9 -and -not $row.plain.ok) { $notes += "COM$n did not open without the prefix: $($row.plain.error_name)" }
    if ($n -ge 10 -and $row.plain.ok) { $notes += "COM$n opened without the prefix" }

    # Another program holds the target: both spellings must be refused, the way a real port is.
    $hold = Start-Hold $c.Port.Port 'syncread'
    try {
        $busy = Invoke-Client $Exe86 @('probe', '--first', '1', '--last', '16') 'probe-x86-busy.json'
        $b = $busy.Json.ports | Where-Object { $_.n -eq $n }
        $detail += "while another program holds COM${n}: prefix gives $($b.prefixed.error_name), plain gives $($b.plain.error_name)"
        if ($b.prefixed.error -ne $ERROR_ACCESS_DENIED) { $notes += "the prefixed open of a held port gave $($b.prefixed.error_name), not ACCESS_DENIED" }
    } finally { Stop-Hold $hold }

    if ($HaveNative) {
        $native = Invoke-Client $ExeNative @('probe', '--first', '1', '--last', '16') "probe-$nativeName-free.json"
        $diffs = Compare-Probe $free.Json $native.Json
        Assert ($diffs.Count -eq 0) "the 32-bit and $nativeName probes differ: $($diffs -join '; ')"
        $detail += "the $nativeName build sees the same"
    }
    if ($notes.Count -gt 0) { throw "UNEXPECTED: $($detail -join '; '); $($notes -join '; ')" }
    $detail
}

function Test-Exercise([string] $Exe, [string] $Arch) {
    $c = Get-WowContext
    $r = Invoke-Client $Exe @('exercise', '--port', $c.Port.Port) "exercise-$Arch.json"
    Assert ($r.Json) "the client answered: $($r.Text)"
    $bad = @($r.Json.steps | Where-Object { -not $_.ok -and $_.required })
    $soft = @($r.Json.steps | Where-Object { -not $_.ok -and -not $_.required })
    $detailLines = @("$($r.Json.summary.steps) steps, $($r.Json.summary.failed) failed, $($r.Json.summary.required_failed) required failed")
    if ($soft.Count -gt 0) { $detailLines += "not required and not ok: " + (($soft | ForEach-Object { "$($_.name) ($($_.error_name))" }) -join ', ') }
    Assert ($bad.Count -eq 0) ("failed: " + (($bad | ForEach-Object { "$($_.name) [$($_.error_name)] $($_.detail)" }) -join ' | '))
    # The lines after SetCommState are what gap G21 is about; say what they were.
    $enable = $r.Json.steps | Where-Object { $_.name -like 'SetCommState 9600 8N1, DTR and RTS enabled' }
    if ($enable) { $detailLines += "after DTR/RTS enable: $($enable.detail -replace '^.*(modem status)', '$1')" }
    $detailLines
}

Add-Test 'W2' 'the 32-bit program makes every call of an old program and gets the answers' {
    Test-Exercise $Exe86 'x86'
}

Add-Test 'W3' "the $nativeName build of the same program does the same" {
    if (-not $HaveNative) { throw "SKIP: $ExeNative is not there" }
    Test-Exercise $ExeNative $nativeName
}

Add-Test 'W4' "the 32-bit and $nativeName results agree step by step" {
    if (-not $HaveNative) { throw "SKIP: $ExeNative is not there" }
    $a = Get-Content -Raw (Join-Path $OutDir 'exercise-x86.json') -ErrorAction SilentlyContinue | ConvertFrom-Json
    $b = Get-Content -Raw (Join-Path $OutDir "exercise-$nativeName.json") -ErrorAction SilentlyContinue | ConvertFrom-Json
    if (-not $a -or -not $b) { throw 'SKIP: W2 or W3 did not leave a result to compare' }
    $diffs = @()
    if ($a.steps.Count -ne $b.steps.Count) { $diffs += "step counts $($a.steps.Count) and $($b.steps.Count)" }
    for ($i = 0; $i -lt [Math]::Min($a.steps.Count, $b.steps.Count); $i++) {
        $x = $a.steps[$i]; $y = $b.steps[$i]
        if ($x.name -ne $y.name -or $x.ok -ne $y.ok -or $x.error -ne $y.error -or $x.detail -ne $y.detail) {
            $diffs += "'$($x.name)': x86 [$($x.ok) $($x.error_name)] $($x.detail)  /  $nativeName [$($y.ok) $($y.error_name)] $($y.detail)"
        }
    }
    Assert ($diffs.Count -eq 0) ($diffs -join ' | ')
    "$($a.steps.Count) steps, identical ok, error and detail"
}

# The echo hub is done with; the hung-program tests make their own ports.
if ($wctx) {
    $wctx.Echo.Dispose()
    $wctx.Hub.Dispose()
    Remove-TestPort $wctx.Port
    $wctx = $null
}

# ---- W5: line errors ----------------------------------------------------------------------
# Waits until the word of the hub state (WowHub.Dtr or WowHub.Rts) is `$Want`, or the program has
# exited, or `$Ms` have passed. The client says where it is by raising DTR and RTS: the hub end
# cannot see what a program does with its errors, but it sees those lines.
function Wait-HubWord($Hub, [int] $Index, [uint32] $Want, [int] $Ms, $Process) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $state = $Hub.GetState()
    while ($state -and $state[$Index] -ne $Want) {
        if ($watch.ElapsedMilliseconds -ge $Ms -or $Process.HasExited) { return $false }
        $next = $Hub.WaitState($state[[WowHub]::Generation], 250)
        if ($next) { $state = $next }
    }
    return ($null -ne $state)
}

# Runs the client in `errors` mode and plays the hub end for it. The hub sends everything in
# order, and waits only for the two lines the client raises: RTS once it has read the parity error
# (the break must not arrive before that, or ClearCommError would report both), and DTR once its
# read is waiting (the framing error must end that read). Returns the client's JSON.
function Invoke-ErrorsRun([string] $Exe, [string] $Arch, $Port, $Hub) {
    $state = $Hub.GetState()
    Assert ($state -and $state[[WowHub]::Dtr] -eq 0 -and $state[[WowHub]::Rts] -eq 0) 'DTR or RTS was still raised from an earlier program'
    $ready = Join-Path $WorkDir "errors-ready-$Arch.json"
    Remove-Item $ready -ErrorAction SilentlyContinue
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = "errors --port $($Port.Port) --ready `"$ready`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $process = [Diagnostics.Process]::Start($psi)
    $output = $process.StandardOutput.ReadToEndAsync()
    $problem = $null
    try {
        $until = (Get-Date).AddSeconds(30)
        while (-not (Test-Path $ready) -and -not $process.HasExited -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100 }
        Assert (Test-Path $ready) "the $Arch program did not report ready (exited: $($process.HasExited))"
        $info = Get-Content -Raw $ready | ConvertFrom-Json
        Assert ([bool]$info.ready) "the $Arch program could not set up $($Port.Port)"

        $e = 0
        # 'a', a parity error whose byte is 'x', and 'b'. The program has asked for the error
        # character, so it reads a?b.
        Assert ($Hub.Write([Text.Encoding]::ASCII.GetBytes('a'), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"
        $e = $Hub.LineError(0x10, 1, 0x78, 5000)
        Assert ($e -eq 0) "the parity error request failed with error $e"
        Assert ($Hub.Write([Text.Encoding]::ASCII.GetBytes('b'), 5000, [ref]$e) -eq 1) "the hub write failed ($e)"

        Assert (Wait-HubWord $Hub ([WowHub]::Rts) 1 25000 $process) 'the program never raised RTS after reading the parity error'
        $e = $Hub.LineError(0x01, 0, 0, 5000)
        Assert ($e -eq 0) "the break request failed with error $e"

        Assert (Wait-HubWord $Hub ([WowHub]::Dtr) 1 25000 $process) 'the program never raised DTR after starting its read'
        $e = $Hub.LineError(0x02, 0, 0, 5000)
        Assert ($e -eq 0) "the framing error request failed with error $e"
        # Behind the error, in the buffer, until the program has called ClearCommError.
        Assert ($Hub.Write([Text.Encoding]::ASCII.GetBytes('ok'), 5000, [ref]$e) -eq 2) "the hub write failed ($e)"
    } catch {
        $problem = $_.Exception.Message
    }
    if (-not $process.WaitForExit(30000)) {
        try { $process.Kill() } catch { }
        $process.WaitForExit(5000) | Out-Null
        $problem = (@($problem, "the $Arch program was still running 30 seconds after the last error") -ne $null) -join '; '
    }
    $text = $output.Result
    $json = $null
    try { $json = $text | ConvertFrom-Json } catch { }
    Set-Content -Path (Join-Path $OutDir "errors-$Arch.json") -Value $text -Encoding UTF8
    $bad = @()
    if ($json) {
        $bad = @($json.steps | Where-Object { -not $_.ok -and $_.required } | ForEach-Object { "$($_.name) [$($_.error_name)] $($_.detail)" })
    }
    if ($problem -or $bad.Count -gt 0 -or $process.ExitCode -ne 0) {
        $parts = @()
        if ($problem) { $parts += $problem }
        if ($bad.Count -gt 0) { $parts += 'failed steps: ' + ($bad -join ' | ') }
        if ($process.ExitCode -ne 0) { $parts += "exit code $($process.ExitCode)" }
        throw "$Arch run: $($parts -join '; ')"
    }
    $json
}

Add-Test 'W5' 'a program that asks for the error character and abort on error sees the parity, break and framing errors the hub raises' {
    $p = New-TestPort
    $hub = Open-Hub $p.Path
    try {
        # An empty error is refused with 87 by a driver that has the request, and with 1 by one that has not.
        if ($hub.LineError(0, 0, 0, 5000) -eq 1) { throw 'SKIP: the installed driver has no line error request (driver version 4)' }
        $x86 = Invoke-ErrorsRun $Exe86 'x86' $p $hub
        $detail = @("x86: $($x86.summary.steps) steps ok, runs as $($x86.arch) under WOW64 $($x86.wow64)")
        if ($HaveNative) {
            $native = Invoke-ErrorsRun $ExeNative $nativeName $p $hub
            $diffs = @()
            if ($x86.steps.Count -ne $native.steps.Count) { $diffs += "step counts $($x86.steps.Count) and $($native.steps.Count)" }
            for ($i = 0; $i -lt [Math]::Min($x86.steps.Count, $native.steps.Count); $i++) {
                $x = $x86.steps[$i]; $y = $native.steps[$i]
                if ($x.name -ne $y.name -or $x.ok -ne $y.ok -or $x.error -ne $y.error -or $x.detail -ne $y.detail) {
                    $diffs += "'$($x.name)': x86 [$($x.ok) $($x.error_name)] $($x.detail)  /  $nativeName [$($y.ok) $($y.error_name)] $($y.detail)"
                }
            }
            Assert ($diffs.Count -eq 0) ("the 32-bit and $nativeName runs differ: " + ($diffs -join ' | '))
            $detail += "${nativeName}: the same $($native.summary.steps) steps"
        } else {
            $detail += "the $nativeName build is not there, so only the 32-bit program ran"
        }
        $detail += 'parity: EV_ERR, CE_RXPARITY, a?b; break: EV_BREAK, CE_BREAK; framing with abort on error: the read ends with 995, CE_FRAME, then ok'
        $detail
    } finally {
        $hub.Dispose()
        Remove-TestPort $p
    }
}

# ---- T: hung programs ---------------------------------------------------------------------
# Kills, or suspends and then kills, a program that holds the port with requests pending.
function Invoke-HoldTest([string] $Scenario, [switch] $Suspend) {
    $p = New-TestPort
    $hub = Open-Hub $p.Path
    $hold = $null
    $detail = @()
    $notes = @()
    try {
        $hold = Start-Hold $p.Port $Scenario
        $state = $hub.GetState()
        Assert ($state -and $state[[WowHub]::AppOpen] -eq 1) 'AppOpen was not set while the program held the port'
        $detail += "$($p.Port) ($($hold.Info.arch), pid $($hold.Pid)); $(Describe-Pending $hold); hub sees TxQueued=$($state[[WowHub]::TxQueued])"
        if ($Scenario -eq 'overlapped') { Assert ($hold.Info.write_pending) 'the write did not block on the full queue' }

        if ($Suspend) {
            $r = [WowProc]::Suspend($hold.Pid)
            Assert ($r -eq 0) "suspending the program failed with $r"
            Start-Sleep -Milliseconds 500
            $state = $hub.GetState()
            $detail += "suspended; AppOpen=$($state[[WowHub]::AppOpen])"
            $second = Open-Again $p.Port
            $detail += "a second open of $($p.Port) from another program returned $($second.error_name) ($($second.error)) after $($second.ms) ms"
            if ($second.error -ne $ERROR_ACCESS_DENIED) { $notes += "the second open gave $($second.error_name), not ACCESS_DENIED" }
        }

        $k = Measure-Kill $hold $hub
        $detail += "killed: AppOpen cleared after $($k.ClearedMs) ms (kill call and clear $($k.TotalMs) ms); process exited: $($k.Exited) after $($k.ExitMs) ms"
        Assert ($k.ClearedMs -ge 0) 'AppOpen was still set 5 seconds after the program was killed'
        Assert ($k.ClearedMs -lt 1000) "AppOpen took $($k.ClearedMs) ms to clear after the kill; the limit is 1000 ms"
        Assert ($k.Exited) 'the killed program was still there 5 seconds later'

        $again = Open-Again $p.Port
        Assert ($again.ok) "the port did not reopen after the kill: $($again.error_name)"
        $detail += 'the port reopens'
        if ($notes.Count -gt 0) { throw "UNEXPECTED: $($detail -join '; '); $($notes -join '; ')" }
        $detail
    } finally {
        Stop-Hold $hold
        $hub.Dispose()
        Remove-TestPort $p
    }
}

Add-Test 'T1' 'a killed program with a pending read and a stalled write frees the port within a second' {
    Invoke-HoldTest 'overlapped'
}

Add-Test 'T2' 'a suspended program keeps the port; a second open is refused; the kill frees it' {
    Invoke-HoldTest 'overlapped' -Suspend
}

Add-Test 'T3' 'a killed program in a synchronous read with no timeout frees the port within a second' {
    Invoke-HoldTest 'syncread'
}

Add-Test 'T4' 'closing the handle in a suspended program from outside frees the port' {
    $p = New-TestPort
    $hub = Open-Hub $p.Path
    $hold = $null
    $detail = @()
    try {
        $hold = Start-Hold $p.Port 'overlapped'
        $detail += "$($p.Port) (pid $($hold.Pid), handle $($hold.Handle)); $(Describe-Pending $hold)"
        $r = [WowProc]::Suspend($hold.Pid)
        Assert ($r -eq 0) "suspending the program failed with $r"
        $closed = [WowProc]::CloseRemoteHandle($hold.Pid, $hold.Handle, 20000)
        $detail += "duplicate with close-source, cancel, close: $closed"
        $cleared = $hub.WaitAppOpen(0, 3000)
        $detail += "AppOpen cleared after $cleared ms (-1 is never)"
        $again = Open-Again $p.Port
        $detail += "a new open of the port: $($again.error_name)"
        $detail += "program still alive and suspended: $(-not $hold.Process.HasExited)"
        if ($closed -notlike 'ok*' -or $cleared -lt 0 -or -not $again.ok) { throw "UNEXPECTED: $($detail -join '; ')" }
        $detail
    } finally {
        Stop-Hold $hold
        $hub.Dispose()
        Remove-TestPort $p
    }
}

Add-Test 'T5' "the driver's revoke request frees a port held by a suspended program, with a write blocked" {
    if ($null -eq $RevokeControl) {
        throw 'SKIP: the driver has no revoke request yet. Define IOCTL_VSSCOM_HUB_REVOKE in drivers/windows/vsscom/public.h (hub end; PR 88 adds it) and ship public.h next to this script, or pass -RevokeCode.'
    }
    $p = New-TestPort
    $hub = Open-Hub $p.Path
    $hold = $null
    $detail = @()
    try {
        $hold = Start-Hold $p.Port 'overlapped'
        Assert ($hold.Info.write_pending) 'the write did not block on the full queue'
        $detail += "$($p.Port) (pid $($hold.Pid)); $(Describe-Pending $hold); revoke code 0x$('{0:X8}' -f $RevokeControl)"
        $r = [WowProc]::Suspend($hold.Pid)
        Assert ($r -eq 0) "suspending the program failed with $r"
        $e = $hub.Control($RevokeControl, 5000)
        Assert ($e -eq 0) "the revoke request failed with error $e"
        $cleared = $hub.WaitAppOpen(0, 1000)
        Assert ($cleared -ge 0) 'AppOpen was still set 1 second after the revoke'
        $detail += "AppOpen cleared after $cleared ms"
        $again = Open-Again $p.Port
        Assert ($again.ok) "the port did not reopen after the revoke: $($again.error_name)"
        $detail += 'the port reopens while the old handle is still in the suspended program'
        # Let the old program run again: its requests on the revoked handle should now end with an error.
        [WowProc]::Resume($hold.Pid) | Out-Null
        Start-Sleep -Seconds 2
        $detail += "after the old program was resumed, its log: $(Get-HoldLog $hold)"
        $detail
    } finally {
        Stop-Hold $hold
        $hub.Dispose()
        Remove-TestPort $p
    }
}

Add-Test 'T6' 'a port removed under a held handle: which slot and COM name come back' {
    $a = New-TestPort -NeedsDemand
    $hub = Open-Hub $a.Path
    $hold = $null
    $b = $null
    $c = $null
    $detail = @()
    $notes = @()
    try {
        $hold = Start-Hold $a.Port 'overlapped'
        $detail += "first port: slot $($a.Slot), $($a.Port), held by pid $($hold.Pid)"
        # The maker lets go: the port is removed, but the program still holds a handle.
        $a.Ctl.Dispose()
        $a.Ctl = $null
        Start-Sleep -Seconds 2
        $listed = Find-VssDemandPort $a.Slot
        $detail += "after the maker closed: the port is $(if ($listed) { 'still listed' } else { 'no longer listed' }); the program is $(if ($hold.Process.HasExited) { 'gone' } else { 'still running' }); SERIALCOMM lists $($a.Port): $(Test-VssSerialComm $a.Port)"
        $detail += "program log: $(Get-HoldLog $hold)"

        $b = New-TestPort -NeedsDemand
        $detail += "made again while the old handle is held: slot $($b.Slot), $($b.Port)"
        if ($b.Slot -eq $a.Slot -or $b.Port -eq $a.Port) { $notes += "the new port took the old slot or COM name ($($b.Slot), $($b.Port))" }
        $b.Ctl.Dispose()
        Wait-VssDemandPort $b.Slot 60 -Gone | Out-Null

        Stop-Hold $hold
        $hub.Dispose()
        $hub = $null
        $gone = Wait-VssDemandPort $a.Slot 60 -Gone
        $commGone = Wait-VssSerialCommGone $a.Port
        $detail += "after the program was killed: the old slot is free: $gone; $($a.Port) left SERIALCOMM: $commGone"
        Assert $gone 'the old slot never became free after the program was killed'

        $c = New-TestPort -NeedsDemand
        $detail += "made once more with nothing held: slot $($c.Slot), $($c.Port)"
        if ($c.Slot -ne $a.Slot) { $notes += "the lowest free slot was $($c.Slot), not the first port's $($a.Slot)" }
        if ($notes.Count -gt 0) { throw "UNEXPECTED: $($detail -join '; '); $($notes -join '; ')" }
        $detail
    } finally {
        Stop-Hold $hold
        if ($hub) { $hub.Dispose() }
        foreach ($port in $a, $b, $c) { if ($port -and $port.Ctl) { $port.Ctl.Dispose() } }
        if ($a) { Wait-VssDemandPort $a.Slot 60 -Gone | Out-Null }
        if ($c) { Wait-VssDemandPort $c.Slot 60 -Gone | Out-Null }
    }
}

Add-Test 'T7' 'restarting the helper service with a program attached' {
    if ($Flavour -ne 'umdf') { throw 'SKIP: the helper service belongs to the user-mode driver (run with -Flavour umdf)' }
    if (-not $HelperRunning) { throw 'SKIP: the helper service is not running (install-driver.ps1 -Flavour umdf -Helper)' }
    $a = New-TestPort -NeedsDemand
    $hub = Open-Hub $a.Path
    $hold = $null
    $b = $null
    $detail = @()
    $problems = @()
    try {
        $hold = Start-Hold $a.Port 'overlapped'
        $detail += "port slot $($a.Slot), $($a.Port), held by pid $($hold.Pid); $(Describe-Pending $hold)"

        $service = Get-Service $HelperServiceName
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $service.Stop()
        $stopped = $true
        try { $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60)) } catch { $stopped = $false }
        $detail += "the service stopped: $stopped after $([int]$watch.Elapsed.TotalSeconds) s"
        if (-not $stopped) { $problems += 'the helper did not stop within 60 s with a program attached' }
        $a.Ctl.Dispose()
        $a.Ctl = $null

        $detail += "after the stop: the program is $(if ($hold.Process.HasExited) { 'gone' } else { 'still running' }); the port is $(if (Find-VssDemandPort $a.Slot) { 'still listed' } else { 'no longer listed' })"
        $state = $null
        try { $state = $hub.GetState() } catch { }
        $detail += "the hub handle $(if ($state) { "still answers, AppOpen=$($state[[WowHub]::AppOpen])" } else { 'no longer answers' })"
        $detail += "program log: $(Get-HoldLog $hold)"

        $service.Refresh()
        if ($service.Status -ne 'Running') {
            $service.Start()
            $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(60))
        }
        $until = (Get-Date).AddSeconds(30)
        while (-not ([IO.Directory]::GetFiles('\\.\pipe\') -contains "\\.\pipe\$HelperPipeName") -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 250 }
        $detail += "the service is running again after $([int]$watch.Elapsed.TotalSeconds) s"

        try {
            $b = New-TestPort -NeedsDemand
            $detail += "a new port: slot $($b.Slot), $($b.Port)"
            if ($b.Slot -eq $a.Slot) { $detail += 'it took the old slot' }
        } catch {
            $problems += "no new port after the restart: $($_.Exception.Message)"
        }

        $k = $null
        if (-not $hold.Process.HasExited) { $hold.Process.Kill() }
        $exited = $hold.Process.WaitForExit(10000)
        $detail += "the program could be killed: $exited"
        if (-not $exited) { $problems += 'the program was still there 10 s after the kill' }
        $hub.Dispose()
        $hub = $null
        $freed = Wait-VssDemandPort $a.Slot 60 -Gone
        $detail += "the old slot is free after the kill: $freed"
        if (-not $freed) { $problems += 'the old slot never became free' }
        if ($problems.Count -gt 0) { throw "$($problems -join '; ') [$($detail -join '; ')]" }
        $detail
    } finally {
        Stop-Hold $hold
        if ($hub) { $hub.Dispose() }
        foreach ($port in $a, $b) { if ($port -and $port.Ctl) { $port.Ctl.Dispose() } }
        $service = Get-Service $HelperServiceName -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Running') { try { $service.Start() } catch { } }
    }
}

Stop-Clients
Write-Result -Path $ResultPath -Phase 'wow64' -Tests $tests
$tests | ForEach-Object { '{0,-10} {1,-4} {2} {3}' -f $_.status, $_.id, $_.name, $_.detail }
if ($tests | Where-Object { $_.status -eq 'fail' }) { exit 1 }
exit 0
