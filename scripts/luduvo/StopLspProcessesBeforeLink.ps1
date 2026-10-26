param(
    [Parameter(Mandatory = $true)]
    [string]$TargetPath,

    [ValidateRange(1, 60000)]
    [int]$TimeoutMilliseconds = 5000
)

$ErrorActionPreference = "Stop"
$target = [System.IO.Path]::GetFullPath($TargetPath)
$comparison = [System.StringComparison]::OrdinalIgnoreCase
$reported = [System.Collections.Generic.HashSet[uint32]]::new()

function Get-TargetProcesses {
    @(
        Get-CimInstance Win32_Process | Where-Object {
            if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                return $false
            }

            try {
                return [System.IO.Path]::GetFullPath($_.ExecutablePath).Equals($target, $comparison)
            }
            catch {
                return $false
            }
        }
    )
}

$deadline = [System.DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
$quietSince = $null

while ([System.DateTime]::UtcNow -lt $deadline) {
    $processes = Get-TargetProcesses
    if ($processes.Count -eq 0) {
        if ($null -eq $quietSince) {
            $quietSince = [System.DateTime]::UtcNow
        }
        elseif (([System.DateTime]::UtcNow - $quietSince).TotalMilliseconds -ge 300) {
            exit 0
        }
    }
    else {
        $quietSince = $null
        foreach ($process in $processes) {
            if ($reported.Add([uint32]$process.ProcessId)) {
                Write-Host "Stopping PID $($process.ProcessId) using $target"
            }
            Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }

    Start-Sleep -Milliseconds 100
}

$remaining = Get-TargetProcesses
if ($remaining.Count -ne 0) {
    $processIds = ($remaining | ForEach-Object { $_.ProcessId }) -join ", "
    Write-Error "Could not stop processes using $target within $TimeoutMilliseconds ms. Remaining PIDs: $processIds"
    exit 1
}
