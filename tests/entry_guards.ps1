param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe,
    [Parameter(Mandatory = $true)][string]$QtBin
)

$ErrorActionPreference = 'Stop'
$env:Path = "$QtBin;$env:Path"

function Invoke-GuardedExecutable(
    [string]$path,
    [int]$expectedExitCode,
    [string]$arguments = ''
) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $path
    $startInfo.Arguments = $arguments
    $startInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($path)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    if (-not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "$path did not reject a direct launch"
    }
    if ($process.ExitCode -ne $expectedExitCode) {
        throw "$path exited with $($process.ExitCode), expected $expectedExitCode"
    }
}

Invoke-GuardedExecutable $ShellExe 1 '--instance=alternate'
Invoke-GuardedExecutable $ProtocolExe 1
Invoke-GuardedExecutable $UiExe 3
Write-Output 'Direct internal executable guards passed'
