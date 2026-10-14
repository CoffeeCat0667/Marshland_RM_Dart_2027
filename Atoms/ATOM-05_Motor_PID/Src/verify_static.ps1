param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$public = Join-Path (Split-Path -Parent $root) 'Public'
$checks = 0

function Assert-Condition([bool] $condition, [string] $message) {
    if (-not $condition) { throw $message }
    $script:checks++
}

function Remove-Literals([string] $source) {
    return [regex]::Replace($source,
        '(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''', ' ')
}

# Text inspection only. No CMake, compiler, CTest, emulator, or module execution.
$headers = @(Get-ChildItem -LiteralPath (Join-Path $root 'Inc') -Recurse -File -Filter '*.hpp')
$sources = @(Get-ChildItem -LiteralPath $PSScriptRoot -File -Filter '*.cpp')
Assert-Condition ($headers.Count -eq 7 -and $sources.Count -eq 5) 'Unexpected module file set'
foreach ($file in @($headers) + @($sources)) {
    $source = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8
    $clean = Remove-Literals $source
    Assert-Condition (-not [regex]::IsMatch($clean, '\bmain\s*\(')) "main entry: $($file.Name)"
    $stack = [System.Collections.Generic.Stack[char]]::new()
    foreach ($character in $clean.ToCharArray()) {
        if ($character -in @('{', '(', '[')) { $stack.Push($character) }
        elseif ($character -in @('}', ')', ']')) {
            $expected = switch ($character) { '}' { '{' } ')' { '(' } ']' { '[' } }
            if ($stack.Count -eq 0 -or $stack.Pop() -ne $expected) {
                throw "Unbalanced delimiters: $($file.Name)"
            }
        }
    }
    Assert-Condition ($stack.Count -eq 0) "Unbalanced delimiters: $($file.Name)"
    foreach ($include in [regex]::Matches($source, '(?m)^\s*#include\s+"([^"]+)"')) {
        $name = $include.Groups[1].Value
        Assert-Condition (-not [regex]::IsMatch($name, 'AppCoordinator|RuntimeCoordinator|StateArbitration\.hpp|atom0[34]')) "Forbidden dependency: $name"
        $paths = @((Join-Path $file.DirectoryName $name),
                   (Join-Path (Join-Path $root 'Inc') $name), (Join-Path $public $name))
        Assert-Condition (@($paths | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }).Count -gt 0) "Missing include: $name"
    }
}

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw -Encoding UTF8
$cmake = [regex]::Replace($cmake, '(?m)#.*$', '')
Assert-Condition ($cmake -match 'set\(ATOM_MODULE_TARGET atom_05_motor_pid\)') 'CMake target changed'
Assert-Condition ($cmake.Contains('Marshland::${ATOM_MODULE_TARGET} ALIAS')) 'Missing alias'
Assert-Condition ($cmake -notmatch '(?i)\b(project|add_executable|target_compile_options)\s*\(') 'Forbidden CMake ownership'
Assert-Condition ($cmake -notmatch 'set\(CMAKE_(CXX_STANDARD|CXX_COMPILER|SYSTEM_PROCESSOR)') 'Global toolchain override'
Assert-Condition ($cmake.Contains('Marshland::PublicHeaders') -and $cmake.Contains('Threads::Threads')) 'Missing contract/thread dependency'

$core = Get-Content -LiteralPath (Join-Path $root 'Src/motor_core.cpp') -Raw -Encoding UTF8
$pidSource = Get-Content -LiteralPath (Join-Path $root 'Src/pid_controller.cpp') -Raw -Encoding UTF8
$config = Get-Content -LiteralPath (Join-Path $root 'Src/motor_config.cpp') -Raw -Encoding UTF8
$types = Get-Content -LiteralPath (Join-Path $root 'Inc/atom05_motor_pid/motor_types.hpp') -Raw -Encoding UTF8
foreach ($method in @('configure', 'start', 'stop', 'configurationVersion', 'onCanFrame',
    'snapshot', 'submitCommand', 'enable', 'disable', 'stopMotor', 'stopAll', 'clearFault',
    'readPidConfig', 'updatePid', 'updateOnce', 'tick', 'findLocked', 'peerLocked', 'now',
    'feedbackStatusLocked', 'revokeLocked', 'revokePairLocked', 'faultLocked', 'commitLocked',
    'stepLocked', 'publishFaults', 'run', 'wake')) {
    Assert-Condition ([regex]::Matches($core, "MotorCore::$method\s*\(").Count -eq 1) "Missing/duplicate definition: $method"
}
Assert-Condition ($types.Contains('kGainLimit = 1000') -and $types.Contains('kPidLimit = 16000')) 'PID range drift'
Assert-Condition ($types.Contains('kActuatorLimitMa = 15000') -and $types.Contains('kEncoderLimitRpm = 7500')) 'Protection range drift'
Assert-Condition ($types.Contains('kSaturationFaultCycles = 5')) 'Saturation count drift'
Assert-Condition ($config.Contains('timing.can_comm_timeout_ms')) 'Missing timeout query'
Assert-Condition ($config.Contains('view.configVersion() != loaded.config_version')) 'Missing configuration version check'
Assert-Condition ($core.Contains('request.source != ControlSource::WEB') -and
    $core.Contains('permit.mode != SystemMode::MANUAL') -and $core.Contains('permit.mode != SystemMode::DEBUG')) 'Missing PID permission checks'
Assert-Condition ($core.Contains('notification_in_flight') -and $core.Contains('stopped_')) 'Missing callback/shutdown guards'
Assert-Condition ($core.Contains('dependencies_.output.forceZero()')) 'Missing generation-exhaustion zero path'
Assert-Condition ($pidSource.Contains('std::isfinite(raw)') -and $pidSource.Contains('std::clamp(raw, -limit, limit)')) 'Missing PID numerical guards'
Assert-Condition ($core.IndexOf('saveMotorPid(') -gt $core.IndexOf('result.applied = true')) 'Persistence precedes hot load'
$rxStart = $core.IndexOf('void MotorCore::onCanFrame(')
$rxEnd = $core.IndexOf('MotorSnapshot MotorCore::snapshot(', $rxStart)
$rx = $core.Substring($rxStart, $rxEnd - $rxStart)
Assert-Condition ($rx -notmatch 'dependencies_\.(output|safety|faults|persistence)\.') 'RX callback invokes external ports'

# Finite mathematical models, not execution/translation of the C++ module.
# They support the manual invariants but are not a proof of source equivalence.
$gainCases = 0
foreach ($p in @(-1001, -1000, -3, 0, 3, 1000, 1001)) {
    foreach ($i in @(-1001, -1000, -1, 0, 1, 1000, 1001)) {
        foreach ($d in @(-1001, -1000, -1, 0, 1, 1000, 1001)) {
            $accepted = $p -ge -1000 -and $p -le 1000 -and
                        $i -ge -1000 -and $i -le 1000 -and $d -ge -1000 -and $d -le 1000
            Assert-Condition ($accepted -eq ([Math]::Max([Math]::Max([Math]::Abs($p), [Math]::Abs($i)), [Math]::Abs($d)) -le 1000)) 'Gain interval model mismatch'
            $gainCases++
        }
    }
}
$permissionCases = 0
foreach ($source in @('NONE', 'SBUS', 'WEB', 'AUTO')) {
    foreach ($mode in @('UNINITIALIZED', 'INITIALIZING', 'INIT_FAILED', 'STANDBY', 'MANUAL',
                        'AUTO_FIRE', 'DEBUG', 'CALIBRATION', 'RECOVERY', 'SOFTWARE_ESTOP', 'FAULT')) {
        foreach ($gate in @($false, $true)) {
            foreach ($valid in @($false, $true)) {
                $accepted = $source -eq 'WEB' -and $mode -in @('MANUAL', 'DEBUG') -and $gate -and $valid
                Assert-Condition (-not $accepted -or ($source -eq 'WEB' -and $gate -and $valid -and $mode -in @('MANUAL', 'DEBUG'))) 'Unauthorized edit in model'
                $permissionCases++
            }
        }
    }
}
for ($raw = 0; $raw -le 65535; $raw++) {
    $signed = if ($raw -ge 32768) { $raw - 65536 } else { $raw }
    $roundtrip = if ($signed -lt 0) { $signed + 65536 } else { $signed }
    Assert-Condition ($signed -ge -32768 -and $signed -le 32767 -and $roundtrip -eq $raw) 'Signed feedback model mismatch'
}
for ($ma = -15000; $ma -le 15000; $ma++) {
    $encoded = [Math]::Truncate([double]$ma * 16384 / 20000)
    $inverse = [Math]::Truncate([double](-$ma) * 16384 / 20000)
    Assert-Condition ([Math]::Abs($encoded) -le 12288 -and $encoded -eq -$inverse) 'Current bound/sign model mismatch'
}
$saturationPaths = 0
for ($path = 0; $path -lt 4096; $path++) {
    $count = 0
    $previous = $false
    $faulted = $false
    for ($step = 0; $step -lt 6; $step++) {
        if ($faulted) { break }
        $saturated = ($path -band (1 -shl (2 * $step))) -ne 0
        $improves = ($path -band (1 -shl (2 * $step + 1))) -ne 0
        if (-not $saturated) { $count = 0 }
        elseif (-not $previous -or $improves) { $count = 1 }
        elseif ($count -lt 5) { $count++ }
        $previous = $true
        $faulted = $count -ge 5
        Assert-Condition ($count -ge 0 -and $count -le 5) 'Saturation counter overflow'
        Assert-Condition (-not $faulted -or ($saturated -and -not $improves)) 'Premature saturation fault'
    }
    $saturationPaths++
}
foreach ($case in @(@{ Times = 5; Expected = 5 }, @{ Times = 4; Expected = 4 })) {
    $count = 0
    for ($i = 0; $i -lt $case.Times; $i++) { $count = [Math]::Min(5, $count + 1) }
    Assert-Condition ($count -eq $case.Expected) 'Plateau saturation threshold mismatch'
}

# Pair model bits: left target, right target, fault latch, safety override.
# Failed commands are separate from stop: ATOM-10 must stop the pair on failure.
$events = @('submit_left', 'submit_right', 'stop_pair', 'fault_left', 'fault_right',
            'clear_fault', 'safety_on', 'safety_off', 'late_pid_step')
$queue = [System.Collections.Generic.Queue[int]]::new()
$seen = [System.Collections.Generic.HashSet[int]]::new()
$queue.Enqueue(0)
[void]$seen.Add(0)
$edges = 0
while ($queue.Count -gt 0) {
    $state = $queue.Dequeue()
    foreach ($event in $events) {
        $next = $state
        switch ($event) {
            'submit_left' { if (($state -band 12) -eq 0) { $next = $state -bor 1 } }
            'submit_right' { if (($state -band 12) -eq 0) { $next = $state -bor 2 } }
            'stop_pair' { $next = $state -band 12 }
            'fault_left' { $next = ($state -band 8) -bor 4 }
            'fault_right' { $next = ($state -band 8) -bor 4 }
            'clear_fault' { $next = $state -band 8 }
            'safety_on' { $next = ($state -band 4) -bor 8 }
            'safety_off' { $next = $state -band 7 }
            'late_pid_step' { $next = $state }
        }
        Assert-Condition ((($next -band 12) -eq 0) -or (($next -band 3) -eq 0)) 'Pair active under fault/safety'
        if ($event -in @('stop_pair', 'fault_left', 'fault_right', 'safety_on', 'clear_fault')) {
            Assert-Condition (($next -band 3) -eq 0) 'Stop/reset resurrected a target'
        }
        if ($event -eq 'late_pid_step') { Assert-Condition ($next -eq $state) 'Late computation resurrected a target' }
        if ($seen.Add($next)) { $queue.Enqueue($next) }
        $edges++
    }
}

Write-Output "STATIC PASS: $($headers.Count) headers, $($sources.Count) sources; $checks assertions"
Write-Output "MODELS: $gainCases gain boundaries; $permissionCases permission cases; 65536 signed words; 30001 currents; $saturationPaths saturation paths; $($seen.Count) pair states/$edges transitions"
Write-Output 'Not performed: C++ compilation, static_assert evaluation, CMake configuration, CTest, emulator, hardware, solver proof of C++ source.'
