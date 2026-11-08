# testtree_smoke.ps1 — verify TestTree.exe builds the three-level tree that
# the killTree E2E depends on, then clean up. Run after `cmake --build build`.
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\tools\TestTree.exe'
$root = Start-Process $exe -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500
$procs = Get-CimInstance Win32_Process -Filter "Name='TestTree.exe'"
$ok = $procs.Count -eq 3
Write-Host ("tree processes: " + $procs.Count + ($ok ? '' : ' (expected 3)'))
if ($ok) {
    $byPid = @{}
    foreach ($p in $procs) { $byPid[[uint32]$p.ProcessId] = $p }
    $ok = $null -ne $byPid[[uint32]$root.Id]
    if ($ok) {
        $children = @($procs | Where-Object { [uint32]$_.ParentProcessId -eq [uint32]$root.Id })
        $ok = $children.Count -eq 1
        if ($ok) {
            $grand = @($procs | Where-Object { [uint32]$_.ParentProcessId -eq [uint32]$children[0].ProcessId })
            $ok = $grand.Count -eq 1
        }
    }
}
Write-Host ($ok ? 'PASS TestTree spawns a 3-level chain' : 'FAIL TestTree tree shape')
# kill the whole tree (the manual killTree target does this via the plugin)
foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
if (-not $root.HasExited) { Stop-Process -Id $root.Id -Force -ErrorAction SilentlyContinue }
exit ($ok ? 0 : 1)
