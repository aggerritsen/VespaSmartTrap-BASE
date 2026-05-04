#include <Arduino.h>
#include "driver/uart.h"
#include "esp_heap_caps.h"

/* =============================
   UART CONFIG — HARD WIRED
   ============================= */
static constexpr uart_port_t UART_PORT = UART_NUM_1;
static constexpr int UART_TX_PIN = 17;
static constexpr int UART_RX_PIN = 18;
static constexpr int UART_BAUD   = 921600;
static constexpr int UART_BUF_SZ = 4096;

void log_memory()
{
    Serial.print("heap_free=");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" heap_min=");
    Serial.print(ESP.getMinFreeHeap());
    Serial.print(" psram=");
    Serial.println(psramFound() ? "YES" : "NO");
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("=======================================");
    Serial.println(" T-SIM7080G-S3 | UART RX PROOF");
    Serial.println("=======================================");

    log_memory();

    /* ---------- UART DRIVER ---------- */
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(
        UART_PORT,
        UART_BUF_SZ,
        UART_BUF_SZ,
        0,
        NULL,
        0
    ));

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT,
        UART_TX_PIN,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_ERROR_CHECK(uart_flush(UART_PORT));

    Serial.println("UART configured:");
    Serial.printf("  PORT   = UART%d\n", UART_PORT);
    Serial.printf("  RX     = GPIO%d\n", UART_RX_PIN);
    Serial.printf("  TX     = GPIO%d\n", UART_TX_PIN);
    Serial.printf("  BAUD   = %d\n", UART_BAUD);
    Serial.println("---------------------------------------");
    Serial.println("Waiting for incoming bytes...");
}

void loop()
{
    uint8_t buf[256];
    int rx_len = uart_read_bytes(
        UART_PORT,
        buf,
        sizeof(buf),
        20 / portTICK_PERIOD_MS
    );

    if (rx_len > 0)
    {
        Serial.printf("\nRX %d bytes:\n", rx_len);

        // HEX dump
        for (int i = 0; i < rx_len; i++)
        {
            Serial.printf("%02X ", buf[i]);
            if ((i + 1) % 16 == 0) Serial.println();
        }
        Serial.println();

        // ASCII view
        Serial.print("ASCII: ");
        for (int i = 0; i < rx_len; i++)
        {
            char c = (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.';
            Serial.print(c);
        }
        Serial.println();
    }
}
