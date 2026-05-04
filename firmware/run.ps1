param(
    [string]$UploadPort = "COM5",
    [string]$MonitorPort = "COM5"
)

pio run -t upload --upload-port $UploadPort
if ($LASTEXITCODE -eq 0) {
    pio device monitor -p $MonitorPort -b 115200
}
