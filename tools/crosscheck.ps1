# crosscheck.ps1 — docs/tests.md §3: every socket row the empty query returns
# must be backed by the OS's own socket lists.
#
# The reference is Get-NetTCPConnection / Get-NetUDPEndpoint, not
# `netstat -ano`: on this machine the stack leaves dwState 0 for many bound
# sockets, and netstat drops those rows entirely. A row matching only on
# protocol + port + pid (the other address family of a dual-stack listener)
# also counts.
$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]
function Note($ok, $label) {
    if ($ok) { Write-Host "PASS $label" } else { Write-Host "FAIL $label"; $script:failures.Add($label) }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'D:\dev\github\winshun-dev\ra9fael.port\port.exe'
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.UseShellExecute = $false
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$proc = [System.Diagnostics.Process]::Start($psi)
$queue = [System.Collections.Queue]::Synchronized((New-Object System.Collections.Queue))
$rs = [runspacefactory]::CreateRunspace(); $rs.Open()
$rs.SessionStateProxy.SetVariable('proc', $proc)
$rs.SessionStateProxy.SetVariable('queue', $queue)
$reader = [powershell]::Create(); $reader.Runspace = $rs
$reader.AddScript('while (-not $proc.HasExited) { $l = $proc.StandardOutput.ReadLine(); if ($null -eq $l) { break }; $queue.Enqueue($l) }') | Out-Null
$reader.BeginInvoke() | Out-Null
function Send($obj) {
    $json = ($obj | ConvertTo-Json -Depth 12 -Compress) + "`n"
    $b = [System.Text.Encoding]::UTF8.GetBytes($json)
    $proc.StandardInput.BaseStream.Write($b, 0, $b.Length)
    $proc.StandardInput.BaseStream.Flush()
}
function Read-Result($wantId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        while ($queue.Count -gt 0) {
            try { $m = ($queue.Dequeue() | ConvertFrom-Json) } catch { continue }
            if ($null -ne $m.id -and $m.id -eq $wantId) { return $m }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

Send @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{ locale = 'zh-CN'; settings = @{} } }
Read-Result 1 | Out-Null
Send @{ jsonrpc = '2.0'; id = 2; method = 'query'; params = @{ queryId = 1; command = 'port'; text = ''; limit = 200 } }
$r = (Read-Result 2).result
Send @{ jsonrpc = '2.0'; id = 9; method = 'shutdown'; params = @{} }
$proc.WaitForExit(3000) | Out-Null

# port.exe rows: "port:tcp:v4:0.0.0.0:135:984" / "port:udp:v6:[::]:53:1234".
$mine = @()
foreach ($row in @($r.items)) {
    $proto = $null; $family = $null; $address = $null; $port = $null; $procId = $null
    if ($row.id -match '^port:(tcp|udp):(v4|v6):(.+):(\d+):(\d+)$') {
        $proto = $Matches[1].ToUpper(); $family = $Matches[2]; $address = $Matches[3]
        $port = [int]$Matches[4]; $procId = [int]$Matches[5]
    } else { continue }
    $endp = if ($address -match ':') { $address } else { $address }
    $mine += [pscustomobject]@{ proto = $proto; family = $family; address = $endp; port = $port; pid = $procId }
}
Write-Host "port.exe rows: $($mine.Count)"

$exact = New-Object System.Collections.Generic.HashSet[string]
$byPort = New-Object System.Collections.Generic.HashSet[string]
$addRef = {
    param($proto, $address, $port, $procId)
    $endp = if ($address -match ':') { "[$address]:$port" } else { "${address}:$port" }
    [void]$exact.Add("$proto|$endp|$procId")
    [void]$byPort.Add("$proto|$port|$procId")
}
foreach ($c in (Get-NetTCPConnection -ErrorAction SilentlyContinue)) {
    if ($c.RemotePort -ne 0) { continue } # listeners only, like the empty query
    & $addRef 'TCP' $c.LocalAddress $c.LocalPort $c.OwningProcess
}
foreach ($e in (Get-NetUDPEndpoint -ErrorAction SilentlyContinue)) {
    & $addRef 'UDP' $e.LocalAddress $e.LocalPort $e.OwningProcess
}
Write-Host "reference exact rows: $($exact.Count)"

$missing = @()
$fallback = 0
foreach ($row in $mine) {
    $endp = if ($row.address -match ':') { "[$($row.address)]:$($row.port)" } else { "$($row.address):$($row.port)" }
    if ($exact.Contains("$($row.proto)|$endp|$($row.pid)")) { continue }
    if ($byPort.Contains("$($row.proto)|$($row.port)|$($row.pid)")) { ++$fallback; continue }
    $missing += "$($row.proto) $endp pid $($row.pid)"
}
Write-Host "matched by port only (dual-stack): $fallback"
Note ($missing.Count -eq 0) ("every port.exe row is backed by the OS socket lists" + $(if ($missing.Count) { " (missing: $($missing -join ', '))" }))

# The empty query covers both families and both protocols.
foreach ($kind in 'v4', 'v6') {
    $n = (@($mine) | Where-Object { $_.family -eq $kind }).Count
    Note ($n -gt 0) "the empty query has $kind rows"
}
foreach ($proto in 'TCP', 'UDP') {
    $n = (@($mine) | Where-Object { $_.proto -eq $proto }).Count
    Note ($n -gt 0) "the empty query has $proto rows"
}

Write-Host "crosscheck: $($failures.Count) failed"
if ($failures.Count -gt 0) { exit 1 }
exit 0
