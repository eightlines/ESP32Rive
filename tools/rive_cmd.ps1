# Sends commands to the board's network control port (TCP) and prints the
# replies. No extra tools needed: plain PowerShell (Windows PowerShell 5 or 7).
#
#   ./tools/rive_cmd.ps1 list
#   ./tools/rive_cmd.ps1 "set formula_input 5"
#   ./tools/rive_cmd.ps1 "set formula_input 5" "get formula_input"
#   ./tools/rive_cmd.ps1 -Address 192.168.1.50 list
#   ./tools/rive_cmd.ps1            # interactive: type commands, empty line quits
#   ./tools/rive_cmd.ps1 -Upload my_design.riv     # plays it now and after resets
#   ./tools/rive_cmd.ps1 -Forget                   # back to the embedded file
#
# Commands: list | get <path> | set <path> <value> | fire <path> |
#           down|move|up <x> <y> | info | help
# Address and Port are named only (-Address, -Port), so every bare argument
# is a command.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Address = "169.254.50.50",
    [int]$Port = 7500,
    [string]$Upload,
    [switch]$Forget,
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Commands
)
$ErrorActionPreference = "Stop"

# Uploads go to the board's web server (port 80), not the command port.
if ($Upload) {
    $path = (Resolve-Path $Upload).Path
    $name = [Uri]::EscapeDataString([IO.Path]::GetFileName($path))
    $bytes = [IO.File]::ReadAllBytes($path)
    Write-Host "Uploading $([IO.Path]::GetFileName($path)) ($($bytes.Length) bytes) to ${Address}..."
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "http://${Address}/riv?name=$name" `
            -Body $bytes -ContentType "application/octet-stream" -TimeoutSec 120
        Write-Host $r.Content
    } catch {
        $resp = $_.Exception.Response
        if ($resp) {
            $reader = New-Object IO.StreamReader($resp.GetResponseStream())
            Write-Host -ForegroundColor Red $reader.ReadToEnd()
        } else {
            Write-Host -ForegroundColor Red "Upload failed: $($_.Exception.Message)"
        }
        exit 1
    }
    if (-not $Commands) { exit 0 }
}
if ($Forget) {
    $r = Invoke-WebRequest -UseBasicParsing -Method Delete -Uri "http://${Address}/riv" -TimeoutSec 30
    Write-Host $r.Content
    if (-not $Commands) { exit 0 }
}

try {
    $client = New-Object Net.Sockets.TcpClient
    $connect = $client.BeginConnect($Address, $Port, $null, $null)
    if (-not $connect.AsyncWaitHandle.WaitOne(3000)) {
        throw "no answer from ${Address}:${Port} within 3 s"
    }
    $client.EndConnect($connect)
} catch {
    Write-Host -ForegroundColor Red ("Could not connect to ${Address}:${Port}: $($_.Exception.Message)`n" +
        "Check the board's log for its 'net: address' line, and that this PC has an " +
        "address on the same network (ipconfig: 169.254.x.x when cabled directly).")
    exit 1
}

$stream = $client.GetStream()
$stream.ReadTimeout = 3000
$writer = New-Object IO.StreamWriter($stream)
$writer.AutoFlush = $true
$writer.NewLine = "`n"
$reader = New-Object IO.StreamReader($stream)

function Send-Command([string]$line) {
    $writer.WriteLine($line)
    # Every reply ends with a line starting "ok" or "error".
    while ($true) {
        $reply = $reader.ReadLine()
        if ($null -eq $reply) { Write-Host "(connection closed)"; return }
        Write-Host $reply
        if ($reply -match '^(ok|error)') { return }
    }
}

try {
    if ($Commands -and $Commands.Count -gt 0) {
        foreach ($c in $Commands) { Send-Command $c }
    } else {
        Write-Host "Connected to ${Address}:${Port}. Type commands (help for a list); empty line quits."
        while ($true) {
            $line = Read-Host "rive"
            if ([string]::IsNullOrWhiteSpace($line)) { break }
            Send-Command $line
        }
    }
} finally {
    $client.Close()
}
