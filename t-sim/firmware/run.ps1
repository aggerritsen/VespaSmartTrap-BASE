param(
    [string]$UploadPort = "COM8",
    [string]$MonitorPort = "COM8"
)

pio run -t upload --upload-port $UploadPort
if ($LASTEXITCODE -eq 0) {
    pio device monitor -p $MonitorPort -b 115200 --filter time
}
