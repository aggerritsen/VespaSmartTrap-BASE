param(
    [string]$UploadPort = "COM4",
    [string]$MonitorPort = "COM4"
)

pio run -t upload --upload-port $UploadPort
if ($LASTEXITCODE -eq 0) {
    pio device monitor -p $MonitorPort -b 115200 --filter time
}
