# Receiver POST

The T-SIM7080G-S3 firmware performs a Power-On Self Test covering:

- CPU, flash, heap and PSRAM dimensions
- SIM7080 AT readiness
- network timestamp
- system time setup
- GNSS command path and optional position fix
- broker/GV2 UART configuration
- SD card mount
- `/post.log` overwrite on boot

The serial monitor prints one delayed POST copy so COM5 can attach after flashing.
