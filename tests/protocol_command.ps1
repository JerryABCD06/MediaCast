param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$instance = 'command-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $ShellExe
$arguments = @(
    "--instance=$instance",
    '--test',
    '--test-persistent',
    '--test-toggle-receiving-after-first-frame',
    "--protocol=$ProtocolExe",
    "--ui=$UiExe"
)
$startInfo.Arguments = ($arguments |
    ForEach-Object { '"' + $_ + '"' }) -join ' '
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
    throw 'Protocol command test timed out'
}
$process.WaitForExit()
$out = $outTask.Result
$err = $errTask.Result
if ($process.ExitCode -ne 0) {
    throw "Shell exited with $($process.ExitCode)`nSTDOUT:`n$out`nSTDERR:`n$err"
}
if ($out -notmatch 'SHELL_RECEIVING_TOGGLE_OK enabled=false') {
    throw "Protocol command was not acknowledged`nSTDOUT:`n$out`nSTDERR:`n$err"
}
if ($out -notmatch 'SHELL_UI_FIRST_FRAME') {
    throw "Foreground Ui did not reach FirstFrame`nSTDOUT:`n$out`nSTDERR:`n$err"
}

Write-Output 'Protocol receiving command test passed'
