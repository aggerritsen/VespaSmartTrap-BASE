# Architecture

VST-BASE is organized around the T-SIM7080G-S3 base unit. Grove Vision AI V2 is a stick-on vision module connected through the custom PCB.

```text
T-SIM7080G-S3
  - UART receive path
  - SD logging
  - modem network time
  - GNSS
  - future uplink

Custom PCB
  - T-SIM7080G-S3 interface
  - GV2 XIAO-slot style connector
  - optional motor / actuator hardware references

Grove Vision AI V2 stick-on module
  - camera
  - YOLO11 model
  - inference metadata / image output
```
