# e2e.ps1 — protocol roundtrip for port.exe (docs/tests.md §2), driven from
# the shell instead of the host UI: initialize → query (miss / hit) →
# two-stage kill → shutdown. The kill target is a child PowerShell holding a
# loopback listener on 18765, so the test never ends a system process.
$ErrorActionPreference = 'Stop'

$exe = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\port.exe'))
$failures = New-Object System.Collections.Generic.List[string]

function Note($ok, $label) {
    if ($ok) { Write-Host "PASS $label" } else { Write-Host "FAIL $label"; $script:failures.Add($label) }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$proc = [System.Diagnostics.Process]::Start($psi)

# One pump per stream on separate runspaces: PowerShell's ReadLineAsync
# rejects overlapping calls, so a blocking pump feeding a synchronized queue
# is the honest way to wait with a timeout.
$queue = [System.Collections.Queue]::Synchronized((New-Object System.Collections.Queue))
foreach ($stream in 'Output', 'Error') {
    $rs = [runspacefactory]::CreateRunspace()
    $rs.Open()
    $rs.SessionStateProxy.SetVariable('proc', $proc)
    $rs.SessionStateProxy.SetVariable('queue', $queue)
    $reader = [powershell]::Create()
    $reader.Runspace = $rs
    $reader.AddScript("while (-not `$proc.HasExited) { `$line = `$proc.Standard$stream.ReadLine(); if (`$null -eq `$line) { break }; `$queue.Enqueue(`$line) }") | Out-Null
    $reader.BeginInvoke() | Out-Null
}

function Send($obj) {
    # Raw bytes, not the StreamWriter: PowerShell's StandardInput writer
    # emits a UTF-8 BOM on the first line, which the plugin rightly rejects
    # as "not JSON".
    $json = ($obj | ConvertTo-Json -Depth 12 -Compress) + "`n"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $proc.StandardInput.BaseStream.Flush()
}

# Poll the queue for the JSON-RPC result with id == $wantId, skipping
# notifications, until the deadline.
function Read-Result($wantId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        while ($queue.Count -gt 0) {
            $line = $queue.Dequeue()
            if (-not $line) { continue }
            try { $msg = $line | ConvertFrom-Json } catch { continue }
            if ($null -ne $msg.id -and $msg.id -eq $wantId) { return $msg }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

# 1. initialize
Send @{ jsonrpc = '2.0'; id = 1; method = 'initialize'; params = @{ locale = 'zh-CN'; settings = @{} } }
$init = Read-Result 1
Note ($null -ne $init -and $init.result.name -eq 'ra9fael.port') 'initialize returns the plugin name'

# 2. invalid ports
Send @{ jsonrpc = '2.0'; id = 2; method = 'query'; params = @{ queryId = 1; command = 'port'; text = '70000'; limit = 100 } }
$r = (Read-Result 2).result
$first = @($r.items)[0]
Note ($first.title -like '*65535*') 'query 70000 shows the range hint'
Note ($null -ne $r.rerun) 'the range hint page still reruns'

Send @{ jsonrpc = '2.0'; id = 3; method = 'query'; params = @{ queryId = 2; command = 'port'; text = 'abc'; limit = 100 } }
$r = (Read-Result 3).result
$first = @($r.items)[0]
Note ($first.title -like '*abc*') 'query abc reads as a name that matches nothing'

# 3. the kill target: a child PowerShell listening on 127.0.0.1:18765
$holder = Start-Process powershell -ArgumentList '-NoProfile', '-Command',
    'New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback,18765) | ForEach-Object { $_.Start(); Start-Sleep 600 }' `
    -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500

# 4. the same holder, found by process name (before the port query: the
# plugin drops an armed row when the query text changes)
Send @{ jsonrpc = '2.0'; id = 45; method = 'query'; params = @{ queryId = 31; command = 'port'; text = 'powershell'; limit = 50 } }
$r = (Read-Result 45).result
$named = @($r.items) | Where-Object { $_.data.pid -eq $holder.Id -and $_.title -like '*18765*' } | Select-Object -First 1
Note ($null -ne $named) 'query powershell finds the holder by name'
$nonMatching = (@($r.items) | Where-Object { $_.data.pid -ne $holder.Id -and $_.data.name -notlike '*powershell*' }).Count
Note ($nonMatching -eq 0) 'the name query returns only matching names'

# 5. the hit query
Send @{ jsonrpc = '2.0'; id = 4; method = 'query'; params = @{ queryId = 3; command = 'port'; text = '18765'; limit = 100 } }
$r = (Read-Result 4).result
$rows = @($r.items)
$hit = $rows | Where-Object { $_.data.pid -eq $holder.Id } | Select-Object -First 1
Note ($null -ne $hit) "query 18765 lists the holder (pid $($holder.Id))"
Note ($hit.title -like 'TCP*18765*') 'the row title names TCP 127.0.0.1:18765'
Note ($hit.data.confirm -eq $false) 'the fresh row is not armed'

# 4b. the copy actions read the row's own data
Send @{ jsonrpc = '2.0'; id = 41; method = 'action'; params = @{ command = 'port'; action = 'copyPid'; text = '18765'; item = @{ id = $hit.id; data = $hit.data } } }
$a = (Read-Result 41).result
Note ($a.effects[0].type -eq 'copy' -and $a.effects[0].text -eq "$($holder.Id)") 'copyPid copies the pid'

Send @{ jsonrpc = '2.0'; id = 42; method = 'action'; params = @{ command = 'port'; action = 'copyRow'; text = '18765'; item = @{ id = $hit.id; data = $hit.data } } }
$a = (Read-Result 42).result
Note ($a.effects[0].type -eq 'copy' -and $a.effects[0].text -like "TCP    127.0.0.1:18765    *    LISTENING    $($holder.Id)") "copyRow copies the netstat line [$($a.effects[0].type)|$($a.effects[0].text)]"

Send @{ jsonrpc = '2.0'; id = 43; method = 'action'; params = @{ command = 'port'; action = 'copyPath'; text = '18765'; item = @{ id = $hit.id; data = $hit.data } } }
$a = (Read-Result 43).result
Note ($a.effects[0].type -eq 'copy' -and $a.effects[0].text -like '*powershell.exe') "copyPath copies the exe path ($($a.effects[0].text))"

Send @{ jsonrpc = '2.0'; id = 44; method = 'action'; params = @{ command = 'port'; action = 'parentChain'; text = '18765'; item = @{ id = $hit.id; data = $hit.data } } }
$a = (Read-Result 44).result
Note ($a.effects[0].type -eq 'copy' -and $a.effects[0].text -like "powershell.exe ($($holder.Id))*") "parentChain starts at the holder ($($a.effects[0].text))"

# 4c removed (moved before the hit query)

# 4d. locating an exe in a folder the user cannot list (a SYSTEM service's):
# copy the path instead of sending Explorer into an "Access is denied" dialog.
$denied = @{ pid = 1; name = 'clash-verge-service.exe'; path = 'C:\System Volume Information\nope.exe'; line = ''; confirm = $false; treeCount = 0 }
Send @{ jsonrpc = '2.0'; id = 46; method = 'action'; params = @{ command = 'port'; action = 'selectExe'; text = '7897'; item = @{ id = 'port:hint:denied'; data = $denied } } }
$a = (Read-Result 46).result
Note ($a.effects[0].type -eq 'copy' -and $a.effects[0].text -eq 'C:\System Volume Information\nope.exe') 'selectExe copies the path instead of opening an unreadable folder'
Note ($null -ne ($a.effects | Where-Object { $_.type -eq 'notify' -and $_.text -like '*权限*' })) 'selectExe explains the missing access'

Send @{ jsonrpc = '2.0'; id = 47; method = 'action'; params = @{ command = 'port'; action = 'openFolder'; text = '7897'; item = @{ id = 'port:hint:denied'; data = $denied } } }
$a = (Read-Result 47).result
Note ($a.effects[0].type -eq 'copy' -and $null -ne ($a.effects | Where-Object { $_.type -eq 'notify' -and $_.text -like '*权限*' })) 'openFolder stops at an unreadable folder too'

# ... and a readable folder whose file is gone.
$gone = @{ pid = 1; name = 'x.exe'; path = 'C:\Windows\System32\no-such-file-xyz.exe'; line = ''; confirm = $false; treeCount = 0 }
Send @{ jsonrpc = '2.0'; id = 48; method = 'action'; params = @{ command = 'port'; action = 'selectExe'; text = '7897'; item = @{ id = 'port:hint:gone'; data = $gone } } }
$a = (Read-Result 48).result
Note ($a.effects[0].type -eq 'copy' -and $null -ne ($a.effects | Where-Object { $_.type -eq 'notify' -and $_.text -like '*不存在*' })) 'selectExe reports a gone file and copies its path'

# 4c. the same holder, found by process name was moved above the hit query.

# 5. two-stage kill: the first Enter only arms
Send @{ jsonrpc = '2.0'; id = 5; method = 'action'; params = @{ command = 'port'; action = 'kill'; text = '18765'; item = @{ id = $hit.id; data = $hit.data } } }
$a = (Read-Result 5).result
Note ($a.effects[0].type -eq 'keep') 'the first kill Enter keeps the window and arms'
Start-Sleep -Milliseconds 500
Note (-not $holder.HasExited) 'nothing is killed by the arming Enter'

# 6. the re-query renders the armed variant
Send @{ jsonrpc = '2.0'; id = 6; method = 'query'; params = @{ queryId = 4; command = 'port'; text = '18765'; limit = 100 } }
$r = (Read-Result 6).result
$confirmRow = @($r.items) | Where-Object { $_.id -eq $hit.id } | Select-Object -First 1
Note ($null -ne $confirmRow -and $confirmRow.data.confirm -eq $true) 'the re-query shows the armed variant'
Note ($confirmRow.icon -like '*dc2626*') 'the armed row carries the red warning icon'

# 7. the second Enter really kills
Send @{ jsonrpc = '2.0'; id = 7; method = 'action'; params = @{ command = 'port'; action = 'kill'; text = '18765'; item = @{ id = $confirmRow.id; data = $confirmRow.data } } }
$a = (Read-Result 7).result
Note ($a.effects[0].type -eq 'notify') 'the confirm Enter notifies'
Note ($null -ne ($a.effects | Where-Object { $_.type -eq 'keep' })) 'the confirm Enter keeps the window'
$holder.WaitForExit(3000) | Out-Null
Note ($holder.HasExited) 'the holder process is gone after the confirm Enter'

# 8. the killed port now misses
Send @{ jsonrpc = '2.0'; id = 8; method = 'query'; params = @{ queryId = 5; command = 'port'; text = '18765'; limit = 100 } }
$r = (Read-Result 8).result
$stillThere = (@($r.items) | Where-Object { $_.data.pid -eq $holder.Id }).Count -gt 0
Note (-not $stillThere) 'after the kill the holder pid is gone from the query'

# 9. the empty query lists real listeners
Send @{ jsonrpc = '2.0'; id = 9; method = 'query'; params = @{ queryId = 6; command = 'port'; text = ''; limit = 100 } }
$r = (Read-Result 9).result
$rows = @($r.items)
Note ($rows.Count -gt 0) "the empty query lists listeners ($($rows.Count) rows)"
$ascending = $true
$last = -1
foreach ($row in $rows) {
    # v6 ids hold a bracketed address whose colons break a plain split.
    if ($row.id -match '^port:(tcp|udp):v[46]:(\[.+\]|\d+\.\d+\.\d+\.\d+):(\d+):\d+$') {
        $port = [int]$Matches[3]
        if ($port -lt $last) { $ascending = $false }
        $last = $port
    }
}
Note $ascending 'the empty query is sorted by port ascending'

# 10. shutdown
Send @{ jsonrpc = '2.0'; id = 99; method = 'shutdown'; params = @{} }
$proc.WaitForExit(3000) | Out-Null
Note ($proc.HasExited) 'shutdown ends the process'

if (-not $proc.HasExited) { $proc.Kill() }
if (-not $holder.HasExited) { Stop-Process -Id $holder.Id -Force }

Write-Host "E2E: $($failures.Count) failed"
if ($failures.Count -gt 0) { $failures | ForEach-Object { Write-Host "  - $_" }; exit 1 }
exit 0
