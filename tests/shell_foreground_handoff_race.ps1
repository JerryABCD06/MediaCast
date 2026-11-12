param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$instance = 'race-' + [guid]::NewGuid().ToString('N').Substring(0, 12)

function Start-Shell {
    $arguments = @(
        "--instance=$instance",
        '--test',
        '--test-hold',
        '--test-persistent',
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
    return [System.Diagnostics.Process]::Start($startInfo)
}

$owner = Start-Shell
try {
    $readyTask = $owner.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(8000)) {
        throw 'Foreground Shell did not become ready'
    }
    if ($readyTask.Result -notmatch '^SHELL_READY ') {
        throw "Unexpected primary Shell output: $($readyTask.Result)"
    }

    $initialLines = ''
    $initialDeadline = [DateTime]::UtcNow.AddSeconds(10)
    $lineTask = $owner.StandardOutput.ReadLineAsync()
    while ([DateTime]::UtcNow -lt $initialDeadline) {
        if (-not $lineTask.Wait(250)) {
            continue
        }
        if ($null -eq $lineTask.Result) {
            break
        }
        $initialLines += $lineTask.Result + "`n"
        if ($initialLines -match 'SHELL_UI_FIRST_FRAME') {
            break
        }
        $lineTask = $owner.StandardOutput.ReadLineAsync()
    }
    if ($initialLines -notmatch 'SHELL_UI_FIRST_FRAME') {
        throw 'Primary foreground startup did not reach FirstFrame'
    }
    Start-Sleep -Milliseconds 500

    $second = Start-Shell
    if (-not $second.WaitForExit(16000)) {
        Stop-Process -Id $second.Id -Force -ErrorAction SilentlyContinue
        throw 'Second Shell did not complete foreground handoff'
    }
    if ($second.ExitCode -ne 0) {
        $secondError = $second.StandardError.ReadToEnd()
        throw "Foreground handoff failed with $($second.ExitCode): $secondError"
    }

    $ownerLines = ''
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $lineTask = $owner.StandardOutput.ReadLineAsync()
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $lineTask.Wait(250)) {
            continue
        }
        if ($null -eq $lineTask.Result) {
            break
        }
        $ownerLines += $lineTask.Result + "`n"
        if ($ownerLines -match 'SHELL_UI_FIRST_FRAME') {
            break
        }
        $lineTask = $owner.StandardOutput.ReadLineAsync()
    }
    if ($ownerLines -notmatch 'SHELL_UI_FIRST_FRAME') {
        throw 'Foreground startup/handoff race did not reach FirstFrame'
    }

    Write-Output 'Foreground Shell handoff race test passed'
}
catch {
    if (-not $owner.HasExited) {
        Stop-Process -Id $owner.Id -Force -ErrorAction SilentlyContinue
    }
    $owner.WaitForExit(5000) | Out-Null
    $ownerError = $owner.StandardError.ReadToEnd()
    throw "$($_.Exception.Message)`nOWNER STDERR:`n$ownerError"
}
finally {
    if (-not $owner.HasExited) {
        Stop-Process -Id $owner.Id -Force -ErrorAction SilentlyContinue
    }
    $owner.WaitForExit(5000) | Out-Null
}
