# Architecture

VST-BASE uses a direct connection between Grove Vision AI V2 and the T-SIM7080G-S3 receiver through the custom T-SIM Motor Shield PCB.

```text
Grove Vision AI V2
  - camera
  - YOLO11 model
  - inference metadata / image output

Custom PCB
  - GV2 XIAO-slot style connector
  - T-SIM7080G-S3 interface
  - optional motor / actuator hardware references

T-SIM7080G-S3
  - UART receive path
  - SD logging
  - modem network time
  - GNSS
  - future uplink
```
