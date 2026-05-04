# T-SIM7080G-S3 Pin Layout (ESP32-S3)

| Header | Pin | GPIO     | Function         | Additional Notes     |
|-------:|----:|----------|------------------|-----------------------|
| P1.1   | 3V3 |          | Power            |                      |
| P1.2   | GND |          | Ground           |                      |
| P1.3   | SDA | GPIO16  | U0CTS            | ADC2_CH5             |
| P1.4   | SCL | GPIO17  | U1TXD            | ADC2_CH6             |
| P1.5   |     | GPIO18  | U1RXD            | ADC2_CH7             |
| P1.6   |     | GPIO08  | TOUCH08          | ADC1_CH7             |
| P1.7   |     | GPIO03  | TOUCH03          | ADC1_CH2             |
| P1.8   |     | GPIO46  |                  |                      |
| P1.9   |  A1 | GPIO09  | TOUCH09 / FSPIHD | ADC1_CH8             |
| P1.10  |  A2 | GPIO10  | TOUCH10 / FSPICS0| ADC1_CH9             |
| P1.11  |  A3 | GPIO11  | TOUCH11 / FSPID  | ADC1_CH10            |
| P1.12  |  A4 | GPIO12  | TOUCH12 / FSPICLK| ADC1_CH1             |
| P1.13  |  A5 | GPIO13  | TOUCH13 / FSPIQ  | ADC1_CH2             |
| P1.14  |  A6 | GPIO14  | TOUCH14 / FSPIWP | ADC1_CH3             |
| P1.15  | GND |          | Ground           |                      |
| P1.16  | DC5 |          | 1.4–3.7V Input   |                      |

| Header | Pin | GPIO     | Function         | Additional Notes     |
|-------:|----:|----------|------------------|-----------------------|
| P2.1   | 3V3 |          | Power            |                      |
| P2.2   | 3V3 |          | Power            |                      |
| P2.3   |     | GPIO01  | TOUCH01          | ADC1_CH0             |
| P2.4   |     | GPIO02  | TOUCH02          | ADC1_CH1             |
| P2.5   | CLK | GPIO43  | CLK_OUT1         |                      |
| P2.6   |     | GPIO44  | CLK_OUT2         |                      |
| P2.7   |     | GPIO37  | FSPI_Q           |                      |
| P2.8   |     | GPIO36  | FSPI_CLK         |                      |
| P2.9   |     | GPIO35  | FSPI_D           |                      |
| P2.10  |     | GPIO00  |                  |                      |
| P2.11  |     | GPIO45  |                  |                      |
| P2.12  |     | GPIO48  |                  |                      |
| P2.13  |     | GPIO47  |                  |                      |
| P2.14  |     | GPIO21  |                  |                      |
| P2.15  | GND |          | Ground           |                      |
| P2.16  | VSYS|          | 5V 0.5A Output   |                      |

## SPI (FSPI) Mapping
| Signal | GPIO |
|--------|------|
| MOSI (FSPID) | GPIO11 |
| MISO (FSPIQ) | GPIO13 |
| SCK  (FSPICLK) | GPIO12 |
| CS   (FSPICS0) | GPIO10 |

## PMU (Power Management Unit)
| Signal | GPIO |
|-----|------|
| SDA | GPIO15 |
| SCL | GPIO07 |
| INT | GPIO06 |

## Camera
| Signal | GPIO |
|-------|------|
| Reset | GPIO18 |
| XCLK  | GPIO08 |
| SIOD  | GPIO02 |
| SIOC  | GPIO01 |
| VSYNC | GPIO16 |
| HREF  | GPIO17 |
| PCLK  | GPIO12 |
| CMYK  |   -1   |
|  Y9   | GPIO09 |
|  Y8   | GPIO10 |
|  Y7   | GPIO11 |
|  Y6   | GPIO13 |
|  Y5   | GPIO21 |
|  Y4   | GPIO48 |
|  Y3   | GPIO47 |
|  Y2   | GPIO14 |

## Modem
| Signal |  GPIO  |
|-------|------|
|  PWR  | GPIO41 |
|  DTR  | GPIO42 |
|  RI   | GPIO03 |
|  RXD  | GPIO04 |
|  TXD  | GPIO05 |


## SD Card
| Signal |  GPIO  |
|-------|------|
|  CMD  | GPIO39 |
|  CLK  | GPIO38 |
|  DATA | GPIO40 |

## 6️⃣ Power domain

| Modem          | Camera            | ESP32S3 | SDCard | Level conversion |
| -------------- | ----------------- | ------- | ------ | ---------------- |
| DC3/BLDO2(GPS) | ALDO1/ALDO2/ALDO4 | DC1     | ALDO3  | BLDO1            |

#define PMU_I2C_SDA_GPIO GPIO_NUM_7
#define PMU_I2C_SCL_GPIO GPIO_NUM_6
