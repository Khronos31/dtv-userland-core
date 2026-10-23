$ErrorActionPreference = 'Stop'
$encoding = [System.Text.UTF8Encoding]::new($false)
$root = (Get-Location).Path
$signalPath = Join-Path $root 'adapters/host_facade/src/px4d_signals.cpp'
$socketPath = Join-Path $root 'adapters/host_runtime/src/control_socket.cpp'
$signalLog = Join-Path $root 'signal-registration-old-behavior.log'
$aceLog = Join-Path $root 'sid-start-old-behavior.log'
$signalOriginal = [System.IO.File]::ReadAllText($signalPath)
$socketOriginal = [System.IO.File]::ReadAllText($socketPath)

function Build-PrimaryFixture([string]$target) {
    & cmake --build build --config Release --target $target --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed for $target with exit $LASTEXITCODE" }
}

function Expect-PrimaryFailure([string]$target, [string]$diagnostic, [string]$logPath) {
    $binary = Join-Path $root "build/Release/$target.exe"
    & $binary *> $logPath
    $fixtureExit = $LASTEXITCODE
    Get-Content $logPath
    Write-Output "MUTATION_PROCESS_EXIT=$fixtureExit"
    if ($fixtureExit -eq 0) { throw "$target unexpectedly passed the old-behavior mutation" }
    if (-not (Select-String -Path $logPath -SimpleMatch $diagnostic -Quiet)) {
        throw "$target failed without the primary diagnostic '$diagnostic'"
    }
    Write-Output "MUTATION_PRIMARY_FIXTURE_FAILED: $target / $diagnostic / exit=$fixtureExit"
}

try {
    $signalOldBehavior = $signalOriginal.Replace(
        'registration_attempts_for_test.fetch_add(1U, std::memory_order_relaxed);',
        'registration_attempts_for_test.fetch_add(1U, std::memory_order_relaxed);' +
        [Environment]::NewLine +
        '    successful_registrations_for_test.fetch_add(1U, std::memory_order_relaxed);')
    if ($signalOldBehavior -eq $signalOriginal) {
        throw 'Signal old-behavior mutation source anchor was not found'
    }
    [System.IO.File]::WriteAllText($signalPath, $signalOldBehavior, $encoding)
    Build-PrimaryFixture 'host_runtime_tests'
    Expect-PrimaryFailure 'host_runtime_tests' `
        'windows_signal_success_counter_after_failure=FAIL' $signalLog
}
finally {
    [System.IO.File]::WriteAllText($signalPath, $signalOriginal, $encoding)
}

try {
    $pattern = '(?s)(\} else if \(header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE\) \{\s*)accepted = false;\s*break;(\s*\})'
    $matches = [System.Text.RegularExpressions.Regex]::Matches($socketOriginal, $pattern)
    if ($matches.Count -ne 1) { throw "Expected one callback-object-ACE fail-closed branch, found $($matches.Count)" }
    $oldObjectParse = '$1trustee = const_cast<DWORD*>(' + [Environment]::NewLine +
        '                    &static_cast<ACCESS_ALLOWED_CALLBACK_OBJECT_ACE*>(raw)->SidStart);$2'
    $socketOldBehavior = [System.Text.RegularExpressions.Regex]::Replace(
        $socketOriginal, $pattern, $oldObjectParse, 1)
    [System.IO.File]::WriteAllText($socketPath, $socketOldBehavior, $encoding)
    Build-PrimaryFixture 'host_runtime_tests'
    Expect-PrimaryFailure 'host_runtime_tests' `
        'windows_sid_start_everyone_mismatch_rejected=FAIL' $aceLog
}
finally {
    [System.IO.File]::WriteAllText($socketPath, $socketOriginal, $encoding)
}

if ([System.IO.File]::ReadAllText($signalPath) -cne $signalOriginal -or
    [System.IO.File]::ReadAllText($socketPath) -cne $socketOriginal) {
    throw 'Mutation sources were not restored byte-for-byte'
}
Build-PrimaryFixture 'host_runtime_tests'
Build-PrimaryFixture 'host_client_integration_tests'
& ctest --test-dir build -C Release -R '^(host_runtime_tests|host_client_integration_tests)$' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Restored primary fixtures failed with exit $LASTEXITCODE" }
Write-Output 'RESTORED_PRIMARY_FIXTURES_PASS'
