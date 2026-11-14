param(
    [Parameter(Mandatory = $true)][string]$ShellExe,
    [Parameter(Mandatory = $true)][string]$ProtocolExe,
    [Parameter(Mandatory = $true)][string]$UiExe,
    [string]$QtBin = '',
    [int]$HoldSeconds = 0,
    [int]$ExpectUiExitSeconds = 0,
    [int]$ExpectPlaybackReconnectSeconds = 0,
    [switch]$ShowWindows,
    [string]$SourceMediaPath = '',
    [string]$SourceMediaUri = ''
)

$ErrorActionPreference = 'Stop'
$instance = 'shadow-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$shellDirectory = [System.IO.Path]::GetDirectoryName($ShellExe)
$protocolLog = Join-Path $shellDirectory "MCast.Protocol.$instance.log"
$uiLog = Join-Path $shellDirectory 'MCast.Ui.Avalonia.log'
$uiLogOffset = if (Test-Path -LiteralPath $uiLog) {
    (Get-Item -LiteralPath $uiLog).Length
} else {
    0
}
$logOffset = 0
$mediaPath = Join-Path $env:TEMP ("mcast-shadow-$instance.wav")
$ownsMediaPath = $true
$mediaUri = ''
$shellProcess = $null
$callbackListener = $null
$testPassed = $false

function Read-AppendedLog {
    if (-not (Test-Path -LiteralPath $protocolLog)) {
        return ''
    }
    $stream = [System.IO.FileStream]::new(
        $protocolLog,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -le $logOffset) {
            return ''
        }
        $stream.Position = $logOffset
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

function Wait-ProtocolLogMatch {
    param(
        [Parameter(Mandatory = $true)][regex]$Pattern,
        [int]$TimeoutMs = 15000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $match = $Pattern.Match((Read-AppendedLog))
        if ($match.Success) {
            return $match
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Protocol log did not match $($Pattern.ToString())`nLOG:`n$(Read-AppendedLog)"
}

function Read-TextSinceOffset {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Offset
    )
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

function Send-TcpHttp {
    param(
        [Parameter(Mandatory = $true)][int]$Port,
        [Parameter(Mandatory = $true)][string]$Request,
        [int]$TimeoutMs = 15000
    )
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.ReceiveTimeout = $TimeoutMs
        $client.SendTimeout = $TimeoutMs
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Request)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $reader = [System.IO.StreamReader]::new(
            $stream,
            [System.Text.Encoding]::UTF8,
            $true,
            4096,
            $true)
        try {
            return $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $client.Dispose()
    }
}

function Receive-CallbackNotify {
    param([int]$TimeoutMs = 5000)
    $accept = $callbackListener.AcceptTcpClientAsync()
    if (-not $accept.Wait($TimeoutMs)) {
        throw 'GENA callback was not received'
    }
    $client = $accept.Result
    try {
        $client.ReceiveTimeout = $TimeoutMs
        $stream = $client.GetStream()
        $buffer = [byte[]]::new(65536)
        $builder = [System.Text.StringBuilder]::new()
        $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (-not $stream.DataAvailable) {
                Start-Sleep -Milliseconds 10
                continue
            }
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) {
                break
            }
            [void]$builder.Append(
                [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read))
            $text = $builder.ToString()
            $headerEnd = $text.IndexOf("`r`n`r`n")
            if ($headerEnd -lt 0) {
                continue
            }
            $contentLength = 0
            $match = [regex]::Match(
                $text.Substring(0, $headerEnd),
                '(?im)^Content-Length:\s*(\d+)\s*$')
            if ($match.Success) {
                $contentLength = [int]$match.Groups[1].Value
            }
            if ($text.Length -ge $headerEnd + 4 + $contentLength) {
                $response = [System.Text.Encoding]::ASCII.GetBytes(
                    "HTTP/1.1 200 OK`r`nContent-Length: 0`r`n" +
                    "Connection: close`r`n`r`n")
                $stream.Write($response, 0, $response.Length)
                $stream.Flush()
                return $text
            }
        }
        throw 'GENA callback request was incomplete'
    } finally {
        $client.Dispose()
    }
}

function Write-SilentWave {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sampleRate = 8000
    $sampleCount = 8000
    $dataSize = $sampleCount * 2
    $bytes = [byte[]]::new(44 + $dataSize)
    $writer = [System.IO.BinaryWriter]::new(
        [System.IO.MemoryStream]::new($bytes))
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $dataSize))
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)
        $writer.Write([uint16]1)
        $writer.Write([uint32]$sampleRate)
        $writer.Write([uint32]($sampleRate * 2))
        $writer.Write([uint16]2)
        $writer.Write([uint16]16)
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32]$dataSize)
    } finally {
        $writer.Dispose()
    }
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    if ($SourceMediaUri) {
        $mediaUri = $SourceMediaUri
        $ownsMediaPath = $false
    } elseif ($SourceMediaPath) {
        $mediaPath = (Resolve-Path -LiteralPath $SourceMediaPath).Path
        $ownsMediaPath = $false
    } else {
        Write-SilentWave $mediaPath
    }
    $callbackListener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback, 0)
    $callbackListener.Start()
    $callbackPort =
        ([System.Net.IPEndPoint]$callbackListener.LocalEndpoint).Port

    $arguments = @(
        "--instance=$instance",
        '--test',
        '--test-hold',
        '--test-persistent',
        '--test-force-receiving-enabled',
        '--test-enable-ssdp',
        "--protocol=$ProtocolExe",
        "--ui=$UiExe"
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ShellExe
    $startInfo.Arguments = ($arguments |
        ForEach-Object { '"' + $_ + '"' }) -join ' '
    $startInfo.WorkingDirectory = $shellDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    if (-not $ShowWindows) {
        $startInfo.WindowStyle =
            [System.Diagnostics.ProcessWindowStyle]::Hidden
    }
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($QtBin) {
        $startInfo.Environment['PATH'] =
            $QtBin + ';' + $startInfo.Environment['PATH']
        $startInfo.Environment['QT_PLUGIN_PATH'] =
            [System.IO.Path]::GetFullPath(
                (Join-Path $QtBin '..\plugins'))
    }
    $shellProcess = [System.Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $shellProcess.StandardOutput.ReadToEndAsync()
    $stderrTask = $shellProcess.StandardError.ReadToEndAsync()

    $httpMatch = Wait-ProtocolLogMatch -Pattern (
        [regex]'HTTP listening on 0\.0\.0\.0:(\d+)')
    $httpPort = [int]$httpMatch.Groups[1].Value
    $ssdpMatch = Wait-ProtocolLogMatch -Pattern (
        [regex]'SSDP listening on port (\d+), location (http://\S+)')
    $ssdpPort = [int]$ssdpMatch.Groups[1].Value
    $location = $ssdpMatch.Groups[2].Value
    if ($ssdpPort -le 0 -or
        $location -ne "http://$(([uri]$location).Host):$httpPort/description.xml") {
        throw "SSDP advertised an unexpected listener: $location"
    }
    $null = Wait-ProtocolLogMatch -Pattern (
        [regex]'Playback UI ready') -TimeoutMs 20000

    $description = Send-TcpHttp -Port $httpPort -Request (
        "GET /description.xml HTTP/1.1`r`n" +
        "Host: 127.0.0.1`r`n" +
        "Connection: close`r`n`r`n")
    if ($description -notmatch '^HTTP/1\.1 200 OK' -or
        $description -notmatch '<friendlyName>[^<]+</friendlyName>' -or
        $description -notmatch '<UDN>uuid:[0-9a-fA-F-]+</UDN>') {
        throw "Unexpected device description:`n$description"
    }

    $subscribe = Send-TcpHttp -Port $httpPort -Request (
        "SUBSCRIBE /AVTransport/event HTTP/1.1`r`n" +
        "Host: 127.0.0.1`r`n" +
        "Callback: <http://127.0.0.1:$callbackPort/notify>`r`n" +
        "NT: upnp:event`r`n" +
        "Timeout: Second-3600`r`n" +
        "Content-Length: 0`r`n" +
        "Connection: close`r`n`r`n")
    if ($subscribe -notmatch '^HTTP/1\.1 200 OK' -or
        $subscribe -notmatch '(?im)^SID:\s*uuid:') {
        throw "Unexpected SUBSCRIBE response:`n$subscribe"
    }
    $initialEvent = Receive-CallbackNotify
    if ($initialEvent -notmatch '<LastChange>' -or
        $initialEvent -notmatch 'TransportState') {
        throw "Initial GENA event was not a LastChange:`n$initialEvent"
    }

    if (-not $mediaUri) {
        $mediaUri = [uri]::new($mediaPath).AbsoluteUri
    }
    $didl = '<DIDL-Lite><item>' +
        '<dc:title xmlns:dc="urn:test">shadow-e2e</dc:title>' +
        '</item></DIDL-Lite>'
    $soap = '<?xml version="1.0"?>' +
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">' +
        '<s:Body><u:SetAVTransportURI ' +
        'xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">' +
        '<InstanceID>0</InstanceID><CurrentURI>' +
        [System.Security.SecurityElement]::Escape($mediaUri) +
        '</CurrentURI><CurrentURIMetaData>' +
        [System.Security.SecurityElement]::Escape($didl) +
        '</CurrentURIMetaData></u:SetAVTransportURI></s:Body></s:Envelope>'
    $setUri = Send-TcpHttp -Port $httpPort -Request (
        "POST /AVTransport/control HTTP/1.1`r`n" +
        "Host: 127.0.0.1`r`n" +
        "User-Agent: MCast-Shadow-E2E`r`n" +
        'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI"' +
        "`r`nContent-Type: text/xml; charset=utf-8`r`n" +
        "Content-Length: $([Text.Encoding]::UTF8.GetByteCount($soap))`r`n" +
        "Connection: close`r`n`r`n$soap") -TimeoutMs 25000
    if ($setUri -notmatch '^HTTP/1\.1 200 OK' -or
        $setUri -notmatch '<u:SetAVTransportURIResponse') {
        throw "SetAVTransportURI did not complete:`n$setUri"
    }

    $postSetEvent = Receive-CallbackNotify -TimeoutMs 5000
    if ($postSetEvent -notmatch '<LastChange>') {
        throw "Post-SetURI GENA event was not a LastChange:`n$postSetEvent"
    }

    $transportBody =
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">' +
        '<s:Body><u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">' +
        '<InstanceID>0</InstanceID></u:GetTransportInfo></s:Body></s:Envelope>'
    $transportInfo = Send-TcpHttp -Port $httpPort -Request (
        "POST /AVTransport/control HTTP/1.1`r`n" +
        "Host: 127.0.0.1`r`n" +
        "User-Agent: MCast-Shadow-E2E`r`n" +
        'SOAPACTION: "urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo"' +
        "`r`nContent-Type: text/xml; charset=utf-8`r`n" +
        "Content-Length: $([Text.Encoding]::UTF8.GetByteCount($transportBody))`r`n" +
        "Connection: close`r`n`r`n$transportBody")
    if ($transportInfo -notmatch '^HTTP/1\.1 200 OK' -or
        $transportInfo -notmatch '<CurrentTransportState>') {
        throw "GetTransportInfo failed after SetURI:`n$transportInfo"
    }

    $testPassed = $true
    if ($HoldSeconds -gt 0) {
        Start-Sleep -Seconds $HoldSeconds
    }
    if ($ExpectPlaybackReconnectSeconds -gt 0) {
        $uiBefore = @(Get-CimInstance Win32_Process |
            Where-Object {
                $_.Name -eq 'MCast.Ui.exe' -and
                $_.CommandLine -like "*$instance*"
            })
        if ($uiBefore.Count -ne 1) {
            throw "Expected one Ui process before reconnect, found $($uiBefore.Count)"
        }
        $null = Wait-ProtocolLogMatch -Pattern (
            [regex]'PlaybackPipe reconnected'
        ) -TimeoutMs ($ExpectPlaybackReconnectSeconds * 1000)
        $uiAfter = @(Get-CimInstance Win32_Process |
            Where-Object {
                $_.Name -eq 'MCast.Ui.exe' -and
                $_.CommandLine -like "*$instance*"
            })
        if ($uiAfter.Count -ne 1 -or
            $uiAfter[0].ProcessId -ne $uiBefore[0].ProcessId) {
            throw 'Ui process changed during PlaybackPipe reconnect'
        }
        $runLog = Read-TextSinceOffset -Path $uiLog -Offset $uiLogOffset
        $readyCount = [regex]::Matches(
            $runLog, 'PlaybackUiReady sent').Count
        $contextCount = [regex]::Matches(
            $runLog, 'Render context attached generation=').Count
        $firstFrameCount = [regex]::Matches(
            $runLog, 'Avalonia first frame presented').Count
        $reconnectCount = [regex]::Matches(
            $runLog, 'Playback reconnect \d+ completed').Count
        if ($readyCount -lt 2 -or
            $contextCount -ne 1 -or
            $firstFrameCount -ne 1 -or
            $reconnectCount -lt 1) {
            throw (
                'Playback reconnect invariants failed: ' +
                "ready=$readyCount context=$contextCount " +
                "firstFrame=$firstFrameCount reconnect=$reconnectCount")
        }
        $protocolRunLog = Read-AppendedLog
        if ([regex]::Matches(
                $protocolRunLog, 'Playback UI ready').Count -lt 2) {
            throw 'Protocol did not receive a second Playback UI ready'
        }
    }
    if ($ExpectUiExitSeconds -gt 0) {
        $deadline = [DateTime]::UtcNow.AddSeconds($ExpectUiExitSeconds)
        do {
            $uiProcesses = Get-CimInstance Win32_Process |
                Where-Object {
                    $_.Name -eq 'MCast.Ui.exe' -and
                    $_.CommandLine -like "*$instance*"
                }
            if (-not $uiProcesses) {
                break
            }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($uiProcesses) {
            throw "Ui did not fail closed within $ExpectUiExitSeconds seconds"
        }
    }
    Write-Output 'DLNA shadow E2E test passed'
} catch {
    if ($shellProcess) {
        if (-not $shellProcess.HasExited) {
            Stop-Process -Id $shellProcess.Id -Force -ErrorAction SilentlyContinue
            [void]$shellProcess.WaitForExit(5000)
        }
        $stdout = if ($stdoutTask) { $stdoutTask.Result } else { '' }
        $stderr = if ($stderrTask) { $stderrTask.Result } else { '' }
        throw "$($_.Exception.Message)`nSHELL STDOUT:`n$stdout`nSHELL STDERR:`n$stderr`nPROTOCOL LOG:`n$(Read-AppendedLog)"
    }
    throw
} finally {
    if ($callbackListener) {
        $callbackListener.Stop()
    }
    if ($shellProcess -and -not $shellProcess.HasExited) {
        Stop-Process -Id $shellProcess.Id -Force -ErrorAction SilentlyContinue
        [void]$shellProcess.WaitForExit(5000)
    }
    if ($ownsMediaPath -and (Test-Path -LiteralPath $mediaPath)) {
        [System.IO.File]::Delete($mediaPath)
    }
    $remaining = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*$instance*" }
    foreach ($process in $remaining) {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($testPassed -and (Test-Path -LiteralPath $protocolLog)) {
        [System.IO.File]::Delete($protocolLog)
    }
}
