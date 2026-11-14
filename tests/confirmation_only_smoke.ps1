param(
    [Parameter(Mandatory = $true)][string]$UiExe
)

$ErrorActionPreference = 'Stop'
$uiExe = (Resolve-Path -LiteralPath $UiExe).Path
$directory = [System.IO.Path]::GetDirectoryName($uiExe)
$logPath = Join-Path $directory 'MCast.Ui.Avalonia.log'
$logOffset = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else {
    0
}

function Read-TextSinceOffset {
    param([string]$Path, [long]$Offset)
    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -le $Offset) {
            return ''
        }
        $stream.Position = $Offset
        $reader = [System.IO.StreamReader]::new(
            $stream,
            [System.Text.Encoding]::UTF8,
            $true,
            1024,
            $true)
        try {
            return $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $uiExe
$startInfo.Arguments =
    '--standalone --confirmation-only --test-takeover-window'
$startInfo.WorkingDirectory = $directory
$startInfo.UseShellExecute = $false
$process = [System.Diagnostics.Process]::Start($startInfo)
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $runLog = ''
    do {
        Start-Sleep -Milliseconds 100
        $runLog = Read-TextSinceOffset `
            -Path $logPath -Offset $logOffset
    } while ($runLog -notmatch 'Avalonia native host started' -and
             [DateTime]::UtcNow -lt $deadline -and
             -not $process.HasExited)
    if ($process.HasExited) {
        throw "confirmation-only Ui exited with $($process.ExitCode)"
    }
    if ($runLog -notmatch 'Avalonia native host started') {
        throw "confirmation-only Ui did not start`n$runLog"
    }
    Start-Sleep -Milliseconds 500
    $runLog = Read-TextSinceOffset `
        -Path $logPath -Offset $logOffset
    $forbidden = @(
        'Render context attached',
        'MPV:',
        'Windows media controls attached',
        'PlaybackBridge'
    )
    foreach ($text in $forbidden) {
        if ($runLog.Contains($text)) {
            throw "confirmation-only Ui created forbidden subsystem: $text"
        }
    }
    if ($runLog -notmatch '--confirmation-only' -or
        $runLog -notmatch '--test-takeover-window') {
        throw "confirmation-only arguments were not received`n$runLog"
    }
    Write-Output 'confirmation-only smoke passed'
} finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        [void]$process.WaitForExit(3000)
    }
}
