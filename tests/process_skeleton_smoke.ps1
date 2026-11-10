param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$instance = 'smoke-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$arguments = @(
    "--instance=$instance",
    '--test',
    '--test-exit-after-first-frame',
    "--protocol=$ProtocolExe",
    "--ui=$UiExe"
)

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $ShellExe
$startInfo.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$startInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($ShellExe)
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $false
$startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [System.Diagnostics.Process]::Start($startInfo)
$outTask = $process.StandardOutput.ReadToEndAsync()
$errTask = $process.StandardError.ReadToEndAsync()

if (-not $process.WaitForExit(15000)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw 'Shell smoke test timed out'
}
$process.WaitForExit()
$exitCode = $process.ExitCode
$out = $outTask.Result
$err = $errTask.Result
if ($null -eq $exitCode -or $exitCode -ne 0) {
    throw "Shell exited with $exitCode`nSTDOUT:`n$out`nSTDERR:`n$err"
}
if ($out -notmatch 'SHELL_READY' -or
    $out -notmatch 'PLAYBACK_SNAPSHOT_SERVED' -or
    $out -notmatch 'PLAYBACK_SNAPSHOT_OK' -or
    $out -notmatch 'SHELL_FIRST_FRAME_OK') {
    throw "Shell did not complete expected handshake`nSTDOUT:`n$out`nSTDERR:`n$err"
}

$childIds = [regex]::Matches($out, 'role=\d+ pid=(\d+)') |
    ForEach-Object { [int]$_.Groups[1].Value }
foreach ($childId in $childIds) {
    $remaining = Get-Process -Id $childId -ErrorAction SilentlyContinue
    if ($remaining) {
        Stop-Process -Id $childId -Force -ErrorAction SilentlyContinue
        throw "orphan child process remained after Shell exit: $childId"
    }
}

Write-Output 'Process skeleton smoke test passed'
