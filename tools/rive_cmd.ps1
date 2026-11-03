# Sends commands to the board's network control port (TCP) and prints the
# replies. No extra tools needed: plain PowerShell (Windows PowerShell 5 or 7).
#
#   ./tools/rive_cmd.ps1 list
#   ./tools/rive_cmd.ps1 "set formula_input 5"
#   ./tools/rive_cmd.ps1 "set formula_input 5" "get formula_input"
#   ./tools/rive_cmd.ps1 -Address 192.168.1.50 list
#   ./tools/rive_cmd.ps1            # interactive: type commands, empty line quits
#
# Commands: list | get <path> | set <path> <value> | fire <path> |
#           down|move|up <x> <y> | info | help
# Address and Port are named only (-Address, -Port), so every bare argument
# is a command.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Address = "169.254.50.50",
    [int]$Port = 7500,
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Commands
)
$ErrorActionPreference = "Stop"

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
