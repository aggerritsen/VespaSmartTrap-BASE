param(
    [string]$Port = "COM16"
)

pio run -t upload --upload-port $Port
if ($LASTEXITCODE -eq 0) {
    pio device monitor -p $Port -b 921600 --filter printable
}
