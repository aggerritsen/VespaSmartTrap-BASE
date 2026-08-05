param(
    [string]$UploadPort = "",
    [string]$MonitorPort = ""
)

function Get-TSimPort {
    $devicesJson = pio device list --json-output
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($devicesJson)) {
        throw "Unable to list PlatformIO devices."
    }

    $devices = $devicesJson | ConvertFrom-Json
    $serialDevices = @($devices | Where-Object { $_.port -match '^COM\d+$' })
    if ($serialDevices.Count -eq 0) {
        throw "No COM ports found."
    }

    $espDevice = @($serialDevices | Where-Object {
        $_.hwid -match 'VID:PID=303A:1001' -or
        $_.description -match 'USB|Serial|Serieel|ESP'
    } | Select-Object -First 1)

    if ($espDevice.Count -gt 0) {
        return $espDevice[0].port
    }

    return $serialDevices[0].port
}

if ([string]::IsNullOrWhiteSpace($UploadPort)) {
    $UploadPort = Get-TSimPort
}

if ([string]::IsNullOrWhiteSpace($MonitorPort)) {
    $MonitorPort = $UploadPort
}

Write-Host "Using upload port $UploadPort"
pio run -t upload --upload-port $UploadPort
if ($LASTEXITCODE -eq 0) {
    Write-Host "Using monitor port $MonitorPort"
    pio device monitor -p $MonitorPort -b 115200 --filter time
}
