param(
    [string]$Address = '192.168.0.36',
    [ValidateRange(1, 65535)][int]$Port = 3232,
    [ValidateRange(5, 120)][int]$Seconds = 60,
    [string]$OutputPath = "$PSScriptRoot/../build/atmosphere-launch-klog.txt"
)
$ErrorActionPreference = 'Stop'
$client = [Net.Sockets.TcpClient]::new()
$writer = $null
try {
    if (!$client.ConnectAsync($Address, $Port).Wait(3000)) { throw 'Klog connection timed out.' }
    $stream = $client.GetStream()
    $stream.ReadTimeout = 1000
    $writer = [IO.File]::CreateText([IO.Path]::GetFullPath($OutputPath))
    $writer.AutoFlush = $true
    Write-Output "CONNECTED: recording kernel log for $Seconds seconds to $OutputPath"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $buffer = [byte[]]::new(16384)
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        if (!$stream.DataAvailable) { Start-Sleep -Milliseconds 100; continue }
        $count = $stream.Read($buffer, 0, $buffer.Length)
        if ($count -eq 0) { break }
        $writer.Write([Text.Encoding]::UTF8.GetString($buffer, 0, $count))
    }
    Write-Output 'Kernel-log recording finished.'
} finally {
    if ($null -ne $writer) { $writer.Dispose() }
    $client.Dispose()
}
