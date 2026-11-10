param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$instance = 'handoff-' + [guid]::NewGuid().ToString('N').Substring(0, 12)

function Start-Shell([string[]]$extraArguments) {
    $arguments = @(
        "--instance=$instance",
        '--test',
        '--test-hold',
        "--protocol=$ProtocolExe",
        "--ui=$UiExe"
    ) + $extraArguments

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

$owner = Start-Shell @('--background')
try {
    $readyTask = $owner.StandardOutput.ReadLineAsync()
    if (-not $readyTask.Wait(8000)) {
        throw 'Primary Shell did not become ready'
    }
    if ($readyTask.Result -notmatch '^SHELL_READY ') {
        throw "Unexpected primary Shell output: $($readyTask.Result)"
    }
    Start-Sleep -Seconds 4
    $unexpectedUi = Get-Process -Name 'MCast.UiSkeleton' -ErrorAction SilentlyContinue |
        Where-Object { $_.StartTime -ge $owner.StartTime }
    if ($unexpectedUi) {
        throw 'A background primary Shell started the Ui process'
    }
    $protocolProcess = Get-Process -Name 'MCast.Protocol' -ErrorAction SilentlyContinue |
        Where-Object { $_.StartTime -ge $owner.StartTime } |
        Select-Object -First 1
    if (-not $protocolProcess) {
        throw 'Protocol exited while the primary Shell was in background mode'
    }

    $second = Start-Shell @()
    if (-not $second.WaitForExit(8000)) {
        Stop-Process -Id $second.Id -Force -ErrorAction SilentlyContinue
        throw 'Second Shell did not hand off and exit'
    }
    if ($second.ExitCode -ne 0) {
        $secondError = $second.StandardError.ReadToEnd()
        throw "Second Shell handoff failed with $($second.ExitCode): $secondError"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $handoffLine = $null
    $ownerLines = ''
    $lineTask = $owner.StandardOutput.ReadLineAsync()
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $lineTask.Wait(250)) {
            continue
        }
        if ($null -eq $lineTask.Result) {
            break
        }
        $ownerLines += $lineTask.Result + "`n"
        if ($lineTask.Result -match '^SHELL_HANDOFF .*action=open-ui accepted=true') {
            $handoffLine = $lineTask.Result
        }
        if ($handoffLine -and $ownerLines -match 'SHELL_UI_FIRST_FRAME') {
            break
        }
        $lineTask = $owner.StandardOutput.ReadLineAsync()
    }
    if (-not $handoffLine) {
        throw 'Primary Shell did not accept handoff'
    }
    if ($ownerLines -notmatch 'SHELL_UI_STARTED') {
        throw "Primary Shell did not start Ui for open-ui handoff"
    }
    if ($ownerLines -notmatch 'SHELL_UI_FIRST_FRAME') {
        throw "Ui started from handoff but never completed Playback/FirstFrame"
    }

    $uiExitDeadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([DateTime]::UtcNow -lt $uiExitDeadline -and
        (Get-Process -Name 'MCast.UiSkeleton' -ErrorAction SilentlyContinue |
            Where-Object { $_.StartTime -ge $owner.StartTime })) {
        Start-Sleep -Milliseconds 100
    }

    $third = Start-Shell @()
    if (-not $third.WaitForExit(8000)) {
        Stop-Process -Id $third.Id -Force -ErrorAction SilentlyContinue
        throw 'Third Shell did not hand off and exit'
    }
    if ($third.ExitCode -ne 0) {
        $thirdError = $third.StandardError.ReadToEnd()
        throw "Third Shell handoff failed with $($third.ExitCode): $thirdError"
    }

    $secondRoundLines = ''
    $lineTask = $owner.StandardOutput.ReadLineAsync()
    $secondRoundDeadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([DateTime]::UtcNow -lt $secondRoundDeadline) {
        if (-not $lineTask.Wait(250)) {
            continue
        }
        if ($null -eq $lineTask.Result) {
            break
        }
        $secondRoundLines += $lineTask.Result + "`n"
        if ($secondRoundLines -match 'SHELL_UI_FIRST_FRAME') {
            break
        }
        $lineTask = $owner.StandardOutput.ReadLineAsync()
    }
    if ($secondRoundLines -notmatch 'SHELL_UI_FIRST_FRAME') {
        throw "Second Ui round did not reach FirstFrame"
    }

    Write-Output 'Shell single-instance handoff test passed (two Ui rounds)'
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
