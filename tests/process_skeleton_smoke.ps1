param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$instance = 'smoke-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('mcast-smoke-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($tempRoot) | Out-Null
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
$startInfo.CreateNoWindow = $true
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
if ($out -notmatch 'SHELL_READY' -or $out -notmatch 'SHELL_FIRST_FRAME_OK') {
    throw "Shell did not complete expected handshake`nSTDOUT:`n$out`nSTDERR:`n$err"
}

[System.IO.Directory]::Delete($tempRoot, $true)
Write-Output 'Process skeleton smoke test passed'
