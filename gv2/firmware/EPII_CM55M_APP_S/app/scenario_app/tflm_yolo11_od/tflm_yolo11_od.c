// version 2026-04-28 I2C disabled to avoid conflicts on those pin

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "powermode_export.h"




// #define WATCH_DOG_TIMEOUT_TH	(20000) //ms
#define WATCH_DOG_TIMEOUT_TH	(500) //ms

#ifdef TRUSTZONE_SEC
#ifdef FREERTOS
/* Trustzone config. */
//
/* FreeRTOS includes. */
//#include "secure_port_macros.h"
#else
#if (__ARM_FEATURE_CMSE & 1) == 0
#error "Need ARMv8-M security extensions"
#elif (__ARM_FEATURE_CMSE & 2) == 0
#error "Compile with --cmse"
#endif
#include "arm_cmse.h"
//#include "veneer_table.h"
//
#endif
#endif

#include "WE2_device.h"

#include "spi_master_protocol.h"
#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"
#include "board.h"
#include "xprintf.h"
#include "tflm_yolo11_od.h"
#include "board.h"
#include "WE2_core.h"
#include "hx_drv_scu.h"
#include "hx_drv_swreg_aon.h"
#ifdef IP_sensorctrl
#include "hx_drv_sensorctrl.h"
#endif
#ifdef IP_xdma
#include "hx_drv_xdma.h"
#include "sensor_dp_lib.h"
#endif
#ifdef IP_cdm
#include "hx_drv_cdm.h"
#endif
#ifdef IP_gpio
#include "hx_drv_gpio.h"
#endif
#include "hx_drv_pmu_export.h"
#include "hx_drv_pmu.h"
#include "powermode.h"
//#include "dp_task.h"
#include "BITOPS.h"

#include "common_config.h"
#include "cisdp_sensor.h"
#include "event_handler.h"
#include "cvapp_yolo11n_ob.h"
#include "memory_manage.h"
#include "hx_drv_watchdog.h"

// I2C slave output to host MCU (ESP32-S3).
// Disabled by default for the "UART-only" setup (define VST_I2C_STATE_TX to enable).
#ifdef VST_I2C_STATE_TX
#include "hx_drv_iic.h"
#endif

// Optional UART output to host MCU (ESP32-S3).
// NOTE: This uses the console UART peripheral (ID 0). [to be verified]
#ifdef VST_UART_STATE_TX
#include "hx_drv_uart.h"
#endif


#ifdef EPII_FPGA
#define DBG_APP_LOG             (1)
#else
#define DBG_APP_LOG             (0)
#endif
#if DBG_APP_LOG
    #define dbg_app_log(fmt, ...)       xprintf(fmt, ##__VA_ARGS__)
#else
    #define dbg_app_log(fmt, ...)
#endif

#define TOTAL_STEP_TICK 1
#define TOTAL_STEP_TICK_DBG_LOG 0

#if TOTAL_STEP_TICK
#define CPU_CLK	0xffffff+1
#endif

#define GROVE_VISION_AI_II

static uint8_t 	g_xdma_abnormal, g_md_detect, g_cdm_fifoerror, g_wdt1_timeout, g_wdt2_timeout,g_wdt3_timeout;
static uint8_t 	g_hxautoi2c_error, g_inp1bitparer_abnormal;
static uint32_t g_dp_event;
static uint8_t 	g_frame_ready;
static uint32_t g_cur_jpegenc_frame;
static uint8_t 	g_time;
static uint8_t g_spi_master_initial_status;
static uint32_t g_use_case;
/*volatile*/ uint32_t jpeg_addr, jpeg_sz;
struct_algoResult algoresult;
struct_fm_algoResult  algoresult_fm;
struct_hp_algoResult  algoresult_pl;
struct_fl_fr_enroll_algoResult  algoresult_fl_fr_enroll;
struct_fl_fr_algoResult  algoresult_fl_fr_infer;
struct_yolov8_ob_algoResult algoresult_yolo11n_ob;
struct_yolov8_ob_algoResult algoresult_yolofastest_ob;
struct_yolov8_pose_algoResult algoresult_yolo11_pose;
static uint32_t g_trans_type;
static uint32_t judge_case_data;
void app_start_state(APP_STATE_E state);
void model_change(void);
void pinmux_init();

// --- Minimal I2C slave "detection state" protocol ---
// The host reads 1 byte from an I2C slave address:
// - 0: none
// - 1: hornet present (class 3)
// - 2: other class present (any class != 3)
// Use 0x63 to avoid collisions while debugging bus visibility.
// Once proven visible, we can switch back to 0x62.
#ifdef VST_I2C_STATE_TX
static const uint8_t kI2cSlaveAddressSlv0 = 0x63;
static volatile uint8_t g_i2c_detection_state = 0;
static uint8_t g_i2c_tx_buf_slv0[1] = {0};
#endif
static const float kMinDetectionConfidence = 0.60f;

#ifdef VST_UART_STATE_TX
static DEV_UART* g_uart_state_tx_dev = NULL;
static uint32_t g_last_uart_tx_frame = 0;

static void uart_state_tx_init_once(void) {
    if (g_uart_state_tx_dev != NULL) {
        return;
    }
    // Use UART1 for the external Grove/UART connector (PB6/PB7 on the module schematic). [to be verified]
    g_uart_state_tx_dev = hx_drv_uart_get_dev(USE_DW_UART_1);
    if (g_uart_state_tx_dev != NULL) {
        // Keep baudrate consistent with existing UART tooling in this scenario. [to be verified]
        g_uart_state_tx_dev->uart_open(UART_BAUDRATE_921600);
    }
}

static void uart_state_tx_send_byte(uint8_t value) {
    uart_state_tx_init_once();
    if (g_uart_state_tx_dev == NULL) {
        return;
    }
    // Framed state message to avoid false positives from UART noise on the host:
    // 'V' 'S' 'T' 'S' + state(1)
    const uint8_t msg[5] = {'V', 'S', 'T', 'S', value};
    (void)g_uart_state_tx_dev->uart_write((const char*)msg, sizeof(msg));
    g_last_uart_tx_frame = g_cur_jpegenc_frame;
}
#endif

#ifdef VST_UART_JPEG_TX
// JPEG capture policy:
// - LED/state decision uses kMinDetectionConfidence (0.60).
// - JPEG send decision mirrors the T-SIM positive filter so GV2 only CRCs/sends useful candidates.
static const float kMinCaptureConfidence = 0.745f;
static const uint8_t kCaptureClassIdx = 3;

// Binary framing to the host MCU:
// Header: 'V' 'S' 'T' 'J' + state(1) + class_idx(1) + conf_u8(1) + bbox_x/y/w/h_u16_le + len_u32_le(4) + crc32_u32_le(4)
static const uint8_t kJpegMagic[4] = {'V', 'S', 'T', 'J'};

// Frame-based rate limit: send at most ~1 JPEG per N frames.
// This approximates 3 images/sec without relying on EVT_INDEX_SENSOR_RTC_FIRE. [to be verified]
static const uint32_t kMinFramesBetweenJpegs = 3;
static const uint32_t kMaxUartJpegPayload = 16U * 1024U;
static uint32_t g_last_jpeg_sent_frame = 0;
static uint8_t g_uart_jpeg_copy[16U * 1024U];

static uint8_t clamp_conf_to_u8(float conf) {
    if (conf <= 0.0f) return 0;
    if (conf >= 1.0f) return 255;
    return (uint8_t)(conf * 255.0f);
}

static void uart_send_bytes(const uint8_t* data, uint32_t len) {
    uart_state_tx_init_once();
    if (g_uart_state_tx_dev == NULL || data == NULL || len == 0) return;
    // Pace writes so the UART driver/FIFO can drain larger 4X JPEG payloads.
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t chunk = (len - pos) > 128 ? 128 : (len - pos);
        (void)g_uart_state_tx_dev->uart_write((const char*)(data + pos), chunk);
        pos += chunk;
        if (pos < len) {
            board_delay_ms(2);
        }
    }
}

static void uart_send_u32_le(uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    uart_send_bytes(b, 4);
}

static void uart_send_u16_le(uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    uart_send_bytes(b, 2);
}

static uint16_t clamp_u32_to_u16(uint32_t v) {
    return v > 65535U ? 65535U : (uint16_t)v;
}

static uint32_t vst_crc32_update(uint32_t crc, const uint8_t* data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t jpeg_payload_len_from_eoi(const uint8_t* data, uint32_t declared_len) {
    if (data == NULL || declared_len < 4) return 0;
    if (data[0] != 0xFF || data[1] != 0xD8) return 0;

    for (uint32_t i = declared_len - 2; i > 1; i--) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            return i + 2;
        }
    }

    return 0;
}

static void uart_send_jpeg_frame(uint8_t state, uint8_t class_idx, uint8_t conf_u8,
                                 uint16_t bbox_x, uint16_t bbox_y, uint16_t bbox_w, uint16_t bbox_h,
                                 uint32_t jpeg_addr_local, uint32_t jpeg_sz_local) {
    if (jpeg_addr_local == 0 || jpeg_sz_local == 0) return;
    if ((g_cur_jpegenc_frame - g_last_jpeg_sent_frame) < kMinFramesBetweenJpegs) return;

    const uint8_t* jpeg_data = (const uint8_t*)jpeg_addr_local;
    uint32_t jpeg_payload_len = jpeg_payload_len_from_eoi(jpeg_data, jpeg_sz_local);
    if (jpeg_payload_len == 0) return;
    if (jpeg_payload_len > kMaxUartJpegPayload) return;

    memcpy(g_uart_jpeg_copy, jpeg_data, jpeg_payload_len);
    uint32_t crc32 = vst_crc32_update(0, g_uart_jpeg_copy, jpeg_payload_len);

    uart_send_bytes(kJpegMagic, sizeof(kJpegMagic));
    uart_send_bytes(&state, 1);
    uart_send_bytes(&class_idx, 1);
    uart_send_bytes(&conf_u8, 1);
    uart_send_u16_le(bbox_x);
    uart_send_u16_le(bbox_y);
    uart_send_u16_le(bbox_w);
    uart_send_u16_le(bbox_h);
    uart_send_u32_le(jpeg_payload_len);
    uart_send_u32_le(crc32);

    uart_send_bytes(g_uart_jpeg_copy, jpeg_payload_len);
    g_last_jpeg_sent_frame = g_cur_jpegenc_frame;
    g_last_uart_tx_frame = g_cur_jpegenc_frame;
}
#endif

#ifdef VST_UART_STATE_TX
// Framed error message to the host MCU:
// 'V' 'S' 'T' 'E' + code(1) + detail(1) + counter_u32_le(4)
static const uint8_t kErrorMagic[4] = {'V', 'S', 'T', 'E'};
static uint32_t g_uart_error_counter = 0;

static void uart_send_error_bytes(const uint8_t* data, uint32_t len) {
    uart_state_tx_init_once();
    if (g_uart_state_tx_dev == NULL || data == NULL || len == 0) return;
    (void)g_uart_state_tx_dev->uart_write((const char*)data, len);
}

static void uart_send_u32_le_raw(uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    uart_send_error_bytes(b, 4);
}

static void uart_send_error_frame(uint8_t code, uint8_t detail) {
    g_uart_error_counter++;
    uart_send_error_bytes(kErrorMagic, sizeof(kErrorMagic));
    uart_send_error_bytes(&code, 1);
    uart_send_error_bytes(&detail, 1);
    uart_send_u32_le_raw(g_uart_error_counter);
    g_last_uart_tx_frame = g_cur_jpegenc_frame;
}

// Framed idle heartbeat to the host MCU:
// 'V' 'S' 'T' 'H' + status(1) + counter_u32_le(4)
// status bit0: camera/frame pipeline is alive.
static const uint8_t kHeartbeatMagic[4] = {'V', 'S', 'T', 'H'};
static uint32_t g_uart_heartbeat_counter = 0;

static void uart_send_heartbeat_frame(uint8_t status) {
    g_uart_heartbeat_counter++;
    uart_send_error_bytes(kHeartbeatMagic, sizeof(kHeartbeatMagic));
    uart_send_error_bytes(&status, 1);
    uart_send_u32_le_raw(g_uart_heartbeat_counter);
    g_last_uart_tx_frame = g_cur_jpegenc_frame;
}

static void uart_send_idle_heartbeat_if_due(uint8_t status) {
    // Frame based so it works without adding another timer dependency.
    // At the normal camera cadence this is roughly 10 seconds of UART silence.
    const uint32_t kHeartbeatIdleFrameInterval = 300U;
    if (g_cur_jpegenc_frame - g_last_uart_tx_frame < kHeartbeatIdleFrameInterval) {
        return;
    }

    uart_send_heartbeat_frame(status);
}

static void uart_report_boot_error_forever(uint8_t code, uint8_t detail) {
    while (1) {
        uart_state_tx_send_byte(code);
        uart_send_error_frame(code, detail);
        board_delay_ms(1000);
    }
}
#endif

#ifdef VST_I2C_STATE_TX
static void i2cs_tx_cb_slv0(void *param) {
    (void)param;
    g_i2c_tx_buf_slv0[0] = g_i2c_detection_state;
    hx_drv_i2cs_interrupt_write(USE_DW_IIC_SLV_0, kI2cSlaveAddressSlv0, g_i2c_tx_buf_slv0, sizeof(g_i2c_tx_buf_slv0), i2cs_tx_cb_slv0);
}

static void i2cs_err_cb_slv0(void *param) {
    (void)param;
    g_i2c_tx_buf_slv0[0] = g_i2c_detection_state;
    hx_drv_i2cs_interrupt_write(USE_DW_IIC_SLV_0, kI2cSlaveAddressSlv0, g_i2c_tx_buf_slv0, sizeof(g_i2c_tx_buf_slv0), i2cs_tx_cb_slv0);
}

static void i2c_slave_init_for_detection_state(void) {
    // I2C slave 0: PA2/PA3.
    // This is the only configuration we keep, to avoid interfering with other board IO
    // (e.g., PB0/PB1 UART console used by tooling). [to be verified]
    hx_drv_scu_set_PA2_pinmux(SCU_PA2_PINMUX_SB_I2C_S_SCL_0, 1);
    hx_drv_scu_set_PA3_pinmux(SCU_PA3_PINMUX_SB_I2C_S_SDA_0, 1);
    hx_drv_i2cs_init(USE_DW_IIC_SLV_0, HX_I2C_HOST_SLV_0_BASE);
    hx_drv_i2cs_set_err_cb(USE_DW_IIC_SLV_0, i2cs_err_cb_slv0);

    g_i2c_detection_state = 0;
    g_i2c_tx_buf_slv0[0] = g_i2c_detection_state;
    hx_drv_i2cs_interrupt_write(USE_DW_IIC_SLV_0, kI2cSlaveAddressSlv0, g_i2c_tx_buf_slv0, sizeof(g_i2c_tx_buf_slv0), i2cs_tx_cb_slv0);
}
#endif

static void update_i2c_detection_state_from_algo_result(void) {
    uint8_t new_state = 0;
    bool saw_capture_candidate = false;
    float best_capture_conf = 0.0f;
    uint16_t best_capture_x = 0;
    uint16_t best_capture_y = 0;
    uint16_t best_capture_w = 0;
    uint16_t best_capture_h = 0;
    for (int i = 0; i < MAX_TRACKED_YOLOV8_ALGO_RES; ++i) {
        const float conf = algoresult_yolo11n_ob.obr[i].confidence;
        const uint8_t class_idx = (uint8_t)algoresult_yolo11n_ob.obr[i].class_idx;
        const uint16_t bbox_x = clamp_u32_to_u16(algoresult_yolo11n_ob.obr[i].bbox.x);
        const uint16_t bbox_y = clamp_u32_to_u16(algoresult_yolo11n_ob.obr[i].bbox.y);
        const uint16_t bbox_w = clamp_u32_to_u16(algoresult_yolo11n_ob.obr[i].bbox.width);
        const uint16_t bbox_h = clamp_u32_to_u16(algoresult_yolo11n_ob.obr[i].bbox.height);
        const bool has_box = bbox_w > 0 && bbox_h > 0;

#ifdef VST_UART_JPEG_TX
        if (has_box && class_idx == kCaptureClassIdx && conf >= kMinCaptureConfidence &&
            (!saw_capture_candidate || conf > best_capture_conf)) {
            saw_capture_candidate = true;
            best_capture_conf = conf;
            best_capture_x = bbox_x;
            best_capture_y = bbox_y;
            best_capture_w = bbox_w;
            best_capture_h = bbox_h;
        }
#endif

        if (conf < kMinDetectionConfidence) {
            continue;
        }
        if (class_idx == 3) {
            new_state = 1;
        } else if (new_state == 0) {
            new_state = 2;
        }
    }
#ifdef VST_I2C_STATE_TX
    g_i2c_detection_state = new_state;
    g_i2c_tx_buf_slv0[0] = g_i2c_detection_state;
#endif

#ifdef VST_UART_STATE_TX
    // Send continuously: makes bring-up robust even if the host misses an edge.
    uart_state_tx_send_byte(new_state);
#endif

#ifdef VST_UART_JPEG_TX
    // Only send JPEG after the target class clears the capture threshold; CRC work happens inside send.
    if (saw_capture_candidate) {
        const uint8_t conf_u8 = clamp_conf_to_u8(best_capture_conf);
        uart_send_jpeg_frame(new_state,
                             kCaptureClassIdx,
                             conf_u8,
                             best_capture_x,
                             best_capture_y,
                             best_capture_w,
                             best_capture_h,
                             jpeg_addr, jpeg_sz);
    }
#endif
}

#ifdef GROVE_VISION_AI_II
/* Init SPI master pin mux (share with SDIO) */
void spi_m_pinmux_cfg(SCU_PINMUX_CFG_T *pinmux_cfg)
{
	pinmux_cfg->pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;        /*!< pin PB2*/
	pinmux_cfg->pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;        /*!< pin PB3*/
	pinmux_cfg->pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;      /*!< pin PB4*/
	pinmux_cfg->pin_pb11 = SCU_PB11_PINMUX_SPI_M_CS;        /*!< pin PB11*/
}
#else
/* Init SPI master pin mux (share with SDIO) */
void spi_m_pinmux_cfg(SCU_PINMUX_CFG_T *pinmux_cfg)
{
	pinmux_cfg->pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;        /*!< pin PB2*/
	pinmux_cfg->pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;        /*!< pin PB3*/
	pinmux_cfg->pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;      /*!< pin PB4*/
	pinmux_cfg->pin_pb5 = SCU_PB5_PINMUX_SPI_M_CS_1;        /*!< pin PB5*/
}
#endif

/*******************************************************************************
 * Code
 ******************************************************************************/
void pinmux_init()
{
	SCU_PINMUX_CFG_T pinmux_cfg;

	hx_drv_scu_get_all_pinmux_cfg(&pinmux_cfg);

	/* Init SPI master pin mux (share with SDIO) */
	spi_m_pinmux_cfg(&pinmux_cfg);

    // Route UART1 to the Grove Vision AI V2 UART pins used by the shield.
    pinmux_cfg.pin_pb6 = SCU_PB6_PINMUX_UART1_RX;
    pinmux_cfg.pin_pb7 = SCU_PB7_PINMUX_UART1_TX;

	hx_drv_scu_set_all_pinmux_cfg(&pinmux_cfg, 1);
}


static void dp_var_int()
{
	g_xdma_abnormal = 0;
	g_md_detect = 0;
	g_cdm_fifoerror = 0;
	g_wdt1_timeout = 0;
	g_wdt2_timeout = 0;
	g_wdt3_timeout = 0;
	g_inp1bitparer_abnormal = 0;
	g_dp_event = 0;
	g_frame_ready = 0;
	g_time = 0;
	g_cur_jpegenc_frame = 0;
	g_hxautoi2c_error = 0;
	g_spi_master_initial_status = 0;
}

void WDG_Reset_ISR_CB (uint32_t event)
{
	uint32_t read_data;
    read_data = hx_drv_watchdog_value(WATCHDOG_ID_0);
    xprintf ("read_data=%d not reset\n", read_data);
	xprintf("CLI_WDG_Reset_ISR_CB event=%d\n", event);
	//hx_drv_watchdog_stop();
}

void SetAlarmPMU() {
	uint32_t id;
	TIMER_CFG_T timer_cfg;
	//TIMER_ERROR_E ret;

#ifdef EPII_FPGA
	timer_cfg.period = 1000;//30000;
#else
	timer_cfg.period = 1000;//30000;
#endif
	timer_cfg.mode = TIMER_MODE_ONESHOT;
	timer_cfg.ctrl = TIMER_CTRL_PMU;
	timer_cfg.state = TIMER_STATE_PMU;
	id = 1;

	//ret = hx_drv_timer_hw_start(id, &timer_cfg, NULL);
	hx_drv_timer_hw_start(id, &timer_cfg, NULL);
}

void SetPSPDNoVid()
{
	PM_PD_NOVIDPRE_CFG_T cfg;
	uint8_t speed,reset, precap, nframeend_ctrl, trigger, retention;
	uint32_t pmu_pad_pa01_mask, pmu_rtc_mask, support_debugdump;
	uint32_t pmu_pad_pa23_mask, pmu_i2cw_mask, pmu_timer_mask, pmu_cmp_mask, pmu_ts_mask;
	uint32_t dcdcpin, freq, cm55mdiv, cm55sdiv, pmu_anti_mask;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;
	PM_CFG_PWR_MODE_E mode;


	speed = SCU_PLL_FREQ_ENABLE;

	reset = 0;
	nframeend_ctrl = 0;

	retention = 0;

	precap = 0;
	pmu_pad_pa01_mask = 0;
	pmu_rtc_mask = 0;
	pmu_pad_pa23_mask = 0;
	pmu_i2cw_mask = 0;
	pmu_timer_mask = 0;
	pmu_cmp_mask = 0;
	pmu_ts_mask = 0;
	trigger = 1;
	support_debugdump = 0;
	dcdcpin = 0;

	freq = 400000000;
	cm55mdiv = SCU_HSCCLKDIV_1;
	cm55sdiv = SCU_LSCCLKDIV_4;

	pmu_anti_mask = 0;
	dbg_app_log("speed=%d,reset=%d,nframeend_ctrl=%d,retention=%d,precap=%d\n", speed,reset,nframeend_ctrl,retention,precap);
	dbg_app_log("ag_mask=0x%x,rtc_mask=0x%x,sb_mask=0x%x,i2cw_mask=0x%x,timer_mask=0x%x,cmp_mask=0x%x,ts_mask=0x%x\n", pmu_pad_pa01_mask,pmu_rtc_mask,pmu_pad_pa23_mask,pmu_i2cw_mask,pmu_timer_mask,pmu_cmp_mask,pmu_ts_mask);
	dbg_app_log("trigger=%d,debug=%d, reset=%d\n", trigger,support_debugdump, reset);
	dbg_app_log("dcdcpin=%d, pmu_anti_mask=0x%x\n", dcdcpin, pmu_anti_mask);
	dbg_app_log("freq=%d, cm55mdiv=%d,cm55sdiv=%d\n", freq, cm55mdiv, cm55sdiv);

	mode = PM_MODE_PS_NOVID_PREROLLING;
	hx_lib_pm_get_defcfg_bymode(&cfg, mode);

	cfg.bootromspeed.bootromclkfreq = speed;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = cm55mdiv;
	cfg.bootromspeed.cm55s_div = cm55sdiv;


	cfg.cm55s_reset = reset;
	cfg.pmu_pad_pa01_mask = pmu_pad_pa01_mask;
	cfg.pmu_rtc_mask = pmu_rtc_mask;
	cfg.pmu_pad_pa23_mask = pmu_pad_pa23_mask;			/**< PMU SB GPIO Interrupt Mask **/
	cfg.pmu_i2cw_mask = pmu_i2cw_mask;			/**< PMU I2C Wakeup Interrupt Mask **/
	cfg.pmu_timer_mask = pmu_timer_mask;			/**< PMU Timer0~5 Wakeup Interrupt Mask  **/
	cfg.pmu_cmp_mask = pmu_cmp_mask;			/**< PMU CMP Wakeup Interrupt Mask  **/
	cfg.pmu_ts_mask = pmu_ts_mask;			/**< PMU TS Wakeup Interrupt Mask  **/
	cfg.pmu_anti_mask = pmu_anti_mask;
	cfg.support_debugdump = support_debugdump;

	cfg.nframeend_ctrl = nframeend_ctrl;	/**< NFrame Control **/

	cfg.tcm_retention = retention;			/**< CM55M TCM Retention**/
	cfg.hscsram_retention[0] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[1] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[2] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[3] = retention;	/**< HSC SRAM Retention**/
	cfg.lscsram_retention = retention;		/**< LSC SRAM Retention**/
	cfg.skip_bootflow.sec_mem_flag = retention;			/**< Skip Boot Flow**/
	cfg.skip_bootflow.first_bl_flag = retention; /*!< First BL Retention */
	cfg.skip_bootflow.cm55m_s_app_flag = retention; /*!< cm55m_s_app Retention */
	cfg.skip_bootflow.cm55m_ns_app_flag = retention; /*!< cm55m_ns_app Retention */
	cfg.skip_bootflow.cm55s_s_app_flag = retention; /*!< cm55s_s_app Retention */
	cfg.skip_bootflow.cm55s_ns_app_flag = retention; /*!< cm55s_ns_app Retention */
	cfg.skip_bootflow.cm55m_model_flag = retention; /*!< cm55m model Retention */
	cfg.skip_bootflow.cm55s_model_flag = retention; /*!< cm55s model Retention */
	cfg.skip_bootflow.cm55m_appcfg_flag = retention; /*!< cm55m appcfg Retention */
	cfg.skip_bootflow.cm55s_appcfg_flag = retention; /*!< cm55s appcfg Retention */
	cfg.skip_bootflow.cm55m_s_app_rwdata_flag = retention;/*!< cm55m_s_app RW Data Retention */
	cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = retention;/*!< cm55m_ns_app RW Data Retention */
	cfg.skip_bootflow.cm55s_s_app_rwdata_flag = retention;/*!< cm55s_s_app RW Data Retention */
	cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = retention;/*!< cm55s_ns_app RW Data Retention */
	cfg.skip_bootflow.secure_debug_flag = retention;
	cfg.support_bootwithcap = precap;		/**< Support capture when boot up**/
	cfg.pmu_dcdc_outpin = dcdcpin;
	cfg.ioret = PM_CFG_PD_IORET_ON;
	cfg.mipi_lane_en = PMU_MIPI_LANE_ALL_DISABLE;
	cfg.sensor_type = PM_SENSOR_TIMING_FVLDLVLD_SHIFT;
	cfg.simo_pd_onoff = PM_SIMO_PD_ONOFF_ON;

	hx_lib_pm_cfg_set(&cfg, NULL, PM_MODE_PS_NOVID_PREROLLING);

	SetAlarmPMU();

	hsc_cfg.hscclk.hscclksrc = SCU_HSCCLKSRC_XTAL24M;
	hsc_cfg.hscclk.hscclkdiv = SCU_HSCCLKDIV_1;
	hsc_cfg.hscd12clksrc = SCU_HSCD12CLKSRC_HSC;
	hsc_cfg.i3chcdiv = SCU_HSCI3CHCLKDIV_1;
	hsc_cfg.sdiodiv = SCU_HSCSDIOCLKDIV_1;
	lsc_cfg.lscclksrc = SCU_LSCCLKSRC_XTAL24M;
	lsc_cfg.lscclkdiv = SCU_LSCCLKDIV_1;

	if(trigger == 1)
	{
		hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYAPP);
	}

}

void SetPSPDNoVid_24M()
{
	PM_PD_NOVIDPRE_CFG_T cfg;
	uint8_t speed,reset, precap, nframeend_ctrl, trigger, retention;
	uint32_t pmu_pad_pa01_mask, pmu_rtc_mask, support_debugdump;
	uint32_t pmu_pad_pa23_mask, pmu_i2cw_mask, pmu_timer_mask, pmu_cmp_mask, pmu_ts_mask;
	uint32_t dcdcpin, freq, cm55mdiv, cm55sdiv, pmu_anti_mask;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;
	PM_CFG_PWR_MODE_E mode;


	speed = SCU_PLL_FREQ_DISABLE;

	reset = 1;
	nframeend_ctrl = 0;

	retention = 0;

	precap = 0;
	pmu_pad_pa01_mask = 0;
	pmu_rtc_mask = 0;
	pmu_pad_pa23_mask = 0;
	pmu_i2cw_mask = 0;
	pmu_timer_mask = 0;
	pmu_cmp_mask = 0;
	pmu_ts_mask = 0;
	trigger = 1;
	support_debugdump = 0;
	dcdcpin = 0;

	freq = 0;
	cm55mdiv = SCU_HSCCLKDIV_1;
	cm55sdiv = SCU_LSCCLKDIV_1;

	pmu_anti_mask = 0;
	dbg_app_log("speed=%d,reset=%d,nframeend_ctrl=%d,retention=%d,precap=%d\n", speed,reset,nframeend_ctrl,retention,precap);
	dbg_app_log("ag_mask=0x%x,rtc_mask=0x%x,sb_mask=0x%x,i2cw_mask=0x%x,timer_mask=0x%x,cmp_mask=0x%x,ts_mask=0x%x\n", pmu_pad_pa01_mask,pmu_rtc_mask,pmu_pad_pa23_mask,pmu_i2cw_mask,pmu_timer_mask,pmu_cmp_mask,pmu_ts_mask);
	dbg_app_log("trigger=%d,debug=%d, reset=%d\n", trigger,support_debugdump, reset);
	dbg_app_log("dcdcpin=%d, pmu_anti_mask=0x%x\n", dcdcpin, pmu_anti_mask);
	dbg_app_log("freq=%d, cm55mdiv=%d,cm55sdiv=%d\n", freq, cm55mdiv, cm55sdiv);

	mode = PM_MODE_PS_NOVID_PREROLLING;
	hx_lib_pm_get_defcfg_bymode(&cfg, mode);

	cfg.bootromspeed.bootromclkfreq = speed;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = cm55mdiv;
	cfg.bootromspeed.cm55s_div = cm55sdiv;


	cfg.cm55s_reset = reset;
	cfg.pmu_pad_pa01_mask = pmu_pad_pa01_mask;
	cfg.pmu_rtc_mask = pmu_rtc_mask;
	cfg.pmu_pad_pa23_mask = pmu_pad_pa23_mask;			/**< PMU SB GPIO Interrupt Mask **/
	cfg.pmu_i2cw_mask = pmu_i2cw_mask;			/**< PMU I2C Wakeup Interrupt Mask **/
	cfg.pmu_timer_mask = pmu_timer_mask;			/**< PMU Timer0~5 Wakeup Interrupt Mask  **/
	cfg.pmu_cmp_mask = pmu_cmp_mask;			/**< PMU CMP Wakeup Interrupt Mask  **/
	cfg.pmu_ts_mask = pmu_ts_mask;			/**< PMU TS Wakeup Interrupt Mask  **/
	cfg.pmu_anti_mask = pmu_anti_mask;
	cfg.support_debugdump = support_debugdump;

	cfg.nframeend_ctrl = nframeend_ctrl;	/**< NFrame Control **/

	cfg.tcm_retention = retention;			/**< CM55M TCM Retention**/
	cfg.hscsram_retention[0] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[1] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[2] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[3] = retention;	/**< HSC SRAM Retention**/
	cfg.lscsram_retention = retention;		/**< LSC SRAM Retention**/
	cfg.skip_bootflow.sec_mem_flag = retention;			/**< Skip Boot Flow**/
	cfg.skip_bootflow.first_bl_flag = retention; /*!< First BL Retention */
	cfg.skip_bootflow.cm55m_s_app_flag = retention; /*!< cm55m_s_app Retention */
	cfg.skip_bootflow.cm55m_ns_app_flag = retention; /*!< cm55m_ns_app Retention */
	cfg.skip_bootflow.cm55s_s_app_flag = retention; /*!< cm55s_s_app Retention */
	cfg.skip_bootflow.cm55s_ns_app_flag = retention; /*!< cm55s_ns_app Retention */
	cfg.skip_bootflow.cm55m_model_flag = retention; /*!< cm55m model Retention */
	cfg.skip_bootflow.cm55s_model_flag = retention; /*!< cm55s model Retention */
	cfg.skip_bootflow.cm55m_appcfg_flag = retention; /*!< cm55m appcfg Retention */
	cfg.skip_bootflow.cm55s_appcfg_flag = retention; /*!< cm55s appcfg Retention */
	cfg.skip_bootflow.cm55m_s_app_rwdata_flag = retention;/*!< cm55m_s_app RW Data Retention */
	cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = retention;/*!< cm55m_ns_app RW Data Retention */
	cfg.skip_bootflow.cm55s_s_app_rwdata_flag = retention;/*!< cm55s_s_app RW Data Retention */
	cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = retention;/*!< cm55s_ns_app RW Data Retention */
	cfg.skip_bootflow.secure_debug_flag = retention;
	cfg.support_bootwithcap = precap;		/**< Support capture when boot up**/
	cfg.pmu_dcdc_outpin = dcdcpin;
	cfg.ioret = PM_CFG_PD_IORET_ON;
	cfg.mipi_lane_en = PMU_MIPI_LANE_ALL_DISABLE;
	cfg.sensor_type = PM_SENSOR_TIMING_FVLDLVLD_SHIFT;
	cfg.simo_pd_onoff = PM_SIMO_PD_ONOFF_ON;

	hx_lib_pm_cfg_set(&cfg, NULL, PM_MODE_PS_NOVID_PREROLLING);

	SetAlarmPMU();

	hsc_cfg.hscclk.hscclksrc = SCU_HSCCLKSRC_XTAL24M;
	hsc_cfg.hscclk.hscclkdiv = SCU_HSCCLKDIV_1;
	hsc_cfg.hscd12clksrc = SCU_HSCD12CLKSRC_HSC;
	hsc_cfg.i3chcdiv = SCU_HSCI3CHCLKDIV_1;
	hsc_cfg.sdiodiv = SCU_HSCSDIOCLKDIV_1;
	lsc_cfg.lscclksrc = SCU_LSCCLKSRC_XTAL24M;
	lsc_cfg.lscclkdiv = SCU_LSCCLKDIV_1;

	if(trigger == 1)
	{
		hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYAPP);
	}

}

void SetPSAudVidonly_24M() {
	PM_PD_VIDAUDPRE_CFG_T cfg;
	uint8_t speed,reset, trigger, retention;
	uint32_t pmu_pad_pa01_mask, pmu_rtc_mask, support_debugdump;
	uint32_t pmu_pad_pa23_mask, pmu_i2cw_mask, pmu_timer_mask, pmu_cmp_mask, pmu_ts_mask;
	uint32_t dcdcpin, freq, cm55mdiv, cm55sdiv, pmu_anti_mask, pmu_senint_mask;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;


	speed = SCU_PLL_FREQ_DISABLE;
	reset = 1;
	retention = 0;

	pmu_pad_pa01_mask = 0;
	pmu_rtc_mask = 0;
	pmu_pad_pa23_mask = 0;
	pmu_i2cw_mask = 0;
	pmu_timer_mask = 0; //TIMER876543210, TIMER MASK1 = 0x2
	pmu_cmp_mask = 0;
	pmu_ts_mask = 0;
	trigger = 1;
	support_debugdump = 0;
	dcdcpin = 0;
	pmu_senint_mask = 0;

	freq = 24000000;
	cm55mdiv = SCU_HSCCLKDIV_1;
	cm55sdiv = SCU_LSCCLKDIV_1;

	pmu_anti_mask = 0;

	cfg.bootromspeed.bootromclkfreq = speed;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = cm55mdiv;
	cfg.bootromspeed.cm55s_div = cm55sdiv;

#ifdef EPII_FPGA
	cfg.sensor_timer = 900;//29900;	 /**< Sensor Timer **/
	cfg.wdt_timer = 1000;	 /**< WDT Timer **/
#else
	cfg.sensor_timer = 900;//29900;	 /**< Sensor Timer **/
	cfg.wdt_timer = 1000;	 /**< WDT Timer **/
#endif
	cfg.nframeend_ctrl = PMU_NFRAMEEND_CTRL_I2C;
	cfg.cm55s_reset = reset;
	cfg.pmu_pad_pa01_mask = pmu_pad_pa01_mask;
	cfg.pmu_rtc_mask = pmu_rtc_mask;
	cfg.pmu_pad_pa23_mask = pmu_pad_pa23_mask;			/**< PMU SB GPIO Interrupt Mask **/
	cfg.pmu_i2cw_mask = pmu_i2cw_mask;			/**< PMU I2C Wakeup Interrupt Mask **/
	cfg.pmu_timer_mask = pmu_timer_mask;			/**< PMU Timer0~5 Wakeup Interrupt Mask  **/
	cfg.pmu_cmp_mask = pmu_cmp_mask;			/**< PMU CMP Wakeup Interrupt Mask  **/
	cfg.pmu_ts_mask = pmu_ts_mask;			/**< PMU TS Wakeup Interrupt Mask  **/
	cfg.pmu_anti_mask = pmu_anti_mask;
	cfg.pmu_mipii2c_noack_mask = 0;
	cfg.pmu_senint_mask = pmu_senint_mask;		/**< PMU SENSOR Interrupt Mask **/
	cfg.support_debugdump = support_debugdump;

	cfg.tcm_retention = retention;			/**< CM55M TCM Retention**/
	cfg.hscsram_retention[0] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[1] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[2] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[3] = retention;	/**< HSC SRAM Retention**/
	cfg.lscsram_retention = retention;		//cfg.lscsram_retention = retention;		/**< LSC SRAM Retention**/
	cfg.skip_bootflow.sec_mem_flag = retention;			/**< Skip Boot Flow**/
	cfg.skip_bootflow.first_bl_flag = retention; /*!< First BL Retention */
	cfg.skip_bootflow.cm55m_s_app_flag = retention; /*!< cm55m_s_app Retention */
	cfg.skip_bootflow.cm55m_ns_app_flag = retention; /*!< cm55m_ns_app Retention */
	cfg.skip_bootflow.cm55s_s_app_flag = retention;//cfg.skip_bootflow.cm55s_s_app_flag = retention; /*!< cm55s_s_app Retention */
	cfg.skip_bootflow.cm55s_ns_app_flag = retention;//cfg.skip_bootflow.cm55s_ns_app_flag = retention; /*!< cm55s_ns_app Retention */
	cfg.skip_bootflow.cm55m_model_flag = retention; /*!< cm55m model Retention */
	cfg.skip_bootflow.cm55s_model_flag = retention;//cfg.skip_bootflow.cm55s_model_flag = retention; /*!< cm55s model Retention */
	cfg.skip_bootflow.cm55m_appcfg_flag = retention; /*!< cm55m appcfg Retention */
	cfg.skip_bootflow.cm55s_appcfg_flag = retention;//cfg.skip_bootflow.cm55s_appcfg_flag = retention; /*!< cm55s appcfg Retention */
	cfg.skip_bootflow.cm55m_s_app_rwdata_flag = retention;/*!< cm55m_s_app RW Data Retention */
	cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = retention;/*!< cm55m_ns_app RW Data Retention */
	cfg.skip_bootflow.cm55s_s_app_rwdata_flag = retention;//cfg.skip_bootflow.cm55s_s_app_rwdata_flag = retention;/*!< cm55s_s_app RW Data Retention */
	cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = retention;//cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = retention;/*!< cm55s_ns_app RW Data Retention */

	cfg.fast_vpr = 0;
	cfg.pmu_dcdc_outpin = dcdcpin;

	hx_lib_pm_cfg_set(&cfg, NULL, PM_MODE_PS_VID_ONLY_PREROLLING);

	SetAlarmPMU();

	hsc_cfg.hscclk.hscclksrc = SCU_HSCCLKSRC_XTAL24M;
	hsc_cfg.hscclk.hscclkdiv = SCU_HSCCLKDIV_1;
	hsc_cfg.hscd12clksrc = SCU_HSCD12CLKSRC_HSC;
	hsc_cfg.i3chcdiv = SCU_HSCI3CHCLKDIV_1;
	hsc_cfg.sdiodiv = SCU_HSCSDIOCLKDIV_1;
	lsc_cfg.lscclksrc = SCU_LSCCLKSRC_XTAL24M;
	lsc_cfg.lscclkdiv = SCU_LSCCLKDIV_1;


	if(trigger == 1)
	{
		hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYAPP);
	}
}
void SetPSAudVidonly()
{
	PM_PD_VIDAUDPRE_CFG_T cfg;
	uint8_t speed,reset, trigger, retention;
	uint32_t pmu_pad_pa01_mask, pmu_rtc_mask, support_debugdump;
	uint32_t pmu_pad_pa23_mask, pmu_i2cw_mask, pmu_timer_mask, pmu_cmp_mask, pmu_ts_mask;
	uint32_t dcdcpin, freq, cm55mdiv, cm55sdiv, pmu_anti_mask, pmu_senint_mask;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;


#ifdef SUPPORT_WARMBOOT_DISPLL
	speed = SCU_PLL_FREQ_DISABLE;
#else
	speed = SCU_PLL_FREQ_ENABLE;
#endif
#ifdef SUPPORT_CM55S_RESET
	reset = 1;
#else
	reset = 0;
#endif
#ifdef SUPPORT_MEM_RETENTION
	retention = 1;
#else
	retention = 0;
#endif

	pmu_pad_pa01_mask = 0;
	pmu_rtc_mask = 0;
	pmu_pad_pa23_mask = 0;
	pmu_i2cw_mask = 0;
	pmu_timer_mask = ~(0x1<<1); //TIMER876543210, TIMER MASK1 = 0x2
	pmu_cmp_mask = 0;
	pmu_ts_mask = 0;
	trigger = 1;
	support_debugdump = 0;
	dcdcpin = 0;
	pmu_senint_mask = 0;

	freq = 400000000;
	cm55mdiv = SCU_HSCCLKDIV_1;
	cm55sdiv = SCU_LSCCLKDIV_4;

	pmu_anti_mask = 0;

	cfg.bootromspeed.bootromclkfreq = speed;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = cm55mdiv;
	cfg.bootromspeed.cm55s_div = cm55sdiv;

#ifdef EPII_FPGA
	cfg.sensor_timer = 900;//29900;	 /**< Sensor Timer **/
	cfg.wdt_timer = 1000;	 /**< WDT Timer **/
#else
	cfg.sensor_timer = 900;//29900;	 /**< Sensor Timer **/
	cfg.wdt_timer = 1000;	 /**< WDT Timer **/
#endif
	cfg.nframeend_ctrl = PMU_NFRAMEEND_CTRL_I2C;
	cfg.cm55s_reset = reset;
	cfg.pmu_pad_pa01_mask = pmu_pad_pa01_mask;
	cfg.pmu_rtc_mask = pmu_rtc_mask;
	cfg.pmu_pad_pa23_mask = pmu_pad_pa23_mask;			/**< PMU SB GPIO Interrupt Mask **/
	cfg.pmu_i2cw_mask = pmu_i2cw_mask;			/**< PMU I2C Wakeup Interrupt Mask **/
	cfg.pmu_timer_mask = pmu_timer_mask;			/**< PMU Timer0~5 Wakeup Interrupt Mask  **/
	cfg.pmu_cmp_mask = pmu_cmp_mask;			/**< PMU CMP Wakeup Interrupt Mask  **/
	cfg.pmu_ts_mask = pmu_ts_mask;			/**< PMU TS Wakeup Interrupt Mask  **/
	cfg.pmu_anti_mask = pmu_anti_mask;
	cfg.pmu_mipii2c_noack_mask = 0;
	cfg.pmu_senint_mask = pmu_senint_mask;		/**< PMU SENSOR Interrupt Mask **/
	cfg.support_debugdump = support_debugdump;

	cfg.tcm_retention = retention;			/**< CM55M TCM Retention**/
	cfg.hscsram_retention[0] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[1] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[2] = retention;	/**< HSC SRAM Retention**/
	cfg.hscsram_retention[3] = retention;	/**< HSC SRAM Retention**/
	cfg.lscsram_retention = 1;		//cfg.lscsram_retention = retention;		/**< LSC SRAM Retention**/
	cfg.skip_bootflow.sec_mem_flag = retention;			/**< Skip Boot Flow**/
	cfg.skip_bootflow.first_bl_flag = retention; /*!< First BL Retention */
	cfg.skip_bootflow.cm55m_s_app_flag = retention; /*!< cm55m_s_app Retention */
	cfg.skip_bootflow.cm55m_ns_app_flag = retention; /*!< cm55m_ns_app Retention */
	cfg.skip_bootflow.cm55s_s_app_flag = 1;//cfg.skip_bootflow.cm55s_s_app_flag = retention; /*!< cm55s_s_app Retention */
	cfg.skip_bootflow.cm55s_ns_app_flag = 1;//cfg.skip_bootflow.cm55s_ns_app_flag = retention; /*!< cm55s_ns_app Retention */
	cfg.skip_bootflow.cm55m_model_flag = retention; /*!< cm55m model Retention */
	cfg.skip_bootflow.cm55s_model_flag = 1;//cfg.skip_bootflow.cm55s_model_flag = retention; /*!< cm55s model Retention */
	cfg.skip_bootflow.cm55m_appcfg_flag = retention; /*!< cm55m appcfg Retention */
	cfg.skip_bootflow.cm55s_appcfg_flag = 1;//cfg.skip_bootflow.cm55s_appcfg_flag = retention; /*!< cm55s appcfg Retention */
	cfg.skip_bootflow.cm55m_s_app_rwdata_flag = retention;/*!< cm55m_s_app RW Data Retention */
	cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = retention;/*!< cm55m_ns_app RW Data Retention */
	cfg.skip_bootflow.cm55s_s_app_rwdata_flag = 1;//cfg.skip_bootflow.cm55s_s_app_rwdata_flag = retention;/*!< cm55s_s_app RW Data Retention */
	cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = 1;//cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = retention;/*!< cm55s_ns_app RW Data Retention */

	cfg.fast_vpr = 0;
	cfg.pmu_dcdc_outpin = dcdcpin;

	hx_lib_pm_cfg_set(&cfg, NULL, PM_MODE_PS_NOVID_PREROLLING);

	SetAlarmPMU();

	hsc_cfg.hscclk.hscclksrc = SCU_HSCCLKSRC_RC24M1M;
	hsc_cfg.hscclk.hscclkdiv = SCU_HSCCLKDIV_1;
	hsc_cfg.hscd12clksrc = SCU_HSCD12CLKSRC_HSC;
	hsc_cfg.i3chcdiv = SCU_HSCI3CHCLKDIV_1;
	hsc_cfg.sdiodiv = SCU_HSCSDIOCLKDIV_1;
	lsc_cfg.lscclksrc = SCU_LSCCLKSRC_RC24M1M;
	lsc_cfg.lscclkdiv = SCU_LSCCLKDIV_1;

	if(trigger == 1)
	{
		hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYAPP);
	}

}


static void dp_app_cv_yolo11n_ob_eventhdl_cb(EVT_INDEX_E event)
{
	uint16_t err;
	int32_t read_status;

	#if DBG_APP_LOG
	dbg_printf(DBG_LESS_INFO, "EVT event = %d\n", event);
	#endif
	g_dp_event = event;

	switch(event)
	{
	case EVT_INDEX_1BITPARSER_ERR:  /*reg_inpparser_fs_cycle_error*/
		hx_drv_inp1bitparser_get_errstatus(&err);
		dbg_printf(DBG_LESS_INFO, "EVT_INDEX_1BITPARSER_ERR err=0x%x\r\n",err);
		hx_drv_inp1bitparser_clear_int();
		hx_drv_inp1bitparser_set_enable(0);
		g_inp1bitparer_abnormal = 1;
		break;
	case EVT_INDEX_EDM_WDT1_TIMEOUT:
		dbg_printf(DBG_LESS_INFO, "EVT_INDEX_EDM_WDT1_TlenIMEOUT\r\n");
		g_wdt1_timeout = 1;
		break;
	case EVT_INDEX_EDM_WDT2_TIMEOUT:
		dbg_printf(DBG_LESS_INFO,"EVT_INDEX_EDM_WDT2_TIMEOUT\r\n");
		g_wdt2_timeout = 1;
		break;
	case EVT_INDEX_EDM_WDT3_TIMEOUT:
		dbg_printf(DBG_LESS_INFO,"EVT_INDEX_EDM_WDT3_TIMEOUT\r\n");
		g_wdt3_timeout = 1;
		break;

	case EVT_INDEX_CDM_FIFO_ERR:
		/*
		 * error happen need CDM timing & TPG setting
		 * 1. SWRESET Datapath
		 * 2. restart streaming flow
		 */
		dbg_printf(DBG_LESS_INFO, "EVT_INDEX_CDM_FIFO_ERR\r\n");
		g_cdm_fifoerror = 1;

		break;

	case EVT_INDEX_XDMA_WDMA1_ABNORMAL:
	case EVT_INDEX_XDMA_WDMA2_ABNORMAL:
	case EVT_INDEX_XDMA_WDMA3_ABNORMAL:
	case EVT_INDEX_XDMA_RDMA_ABNORMAL:
		/*
		 * error happen need
		 * 1. SWRESET Datapath
		 * 2. restart streaming flow
		 */
		dbg_printf(DBG_LESS_INFO, "EVT_INDEX_XDMA_WDMA123_ABNORMAL or EVT_INDEX_XDMA_RDMA_ABNORMAL\r\n");
		g_xdma_abnormal = 1;
		break;

	case EVT_INDEX_CDM_MOTION:
		/*
		 * app anything want to do
		 * */
		dbg_printf(DBG_LESS_INFO, "Motion Detect\n");
		g_md_detect = 1;
		break;
	case EVT_INDEX_XDMA_FRAME_READY:
		g_cur_jpegenc_frame++;
#ifdef VST_UART_STATE_TX
        uart_send_idle_heartbeat_if_due(1);
#endif
    	g_frame_ready = 1;
		dbg_printf(DBG_LESS_INFO, "SENSORDPLIB_STATUS_XDMA_FRAME_READY %d \n", g_cur_jpegenc_frame);
		break;

	case EVT_INDEX_SENSOR_RTC_FIRE:
		g_time++;
		break;
	case EVT_INDEX_HXAUTOI2C_ERR:
		dbg_printf(DBG_LESS_INFO,"EVT_INDEX_HXAUTOI2C_ERR\r\n");
		g_hxautoi2c_error = 1;
		break;
	default:
		dbg_printf(DBG_LESS_INFO,"Other Event %d\n", event);
		break;
	}


	if(g_frame_ready == 1)
	{
		g_frame_ready = 0;

				hx_drv_swreg_aon_get_appused1(&judge_case_data);
// #ifdef WATCHDOG_VERSION
// 		if(EPII_get_memory(0x5610F02c)==g_use_case)
// 			hx_drv_watchdog_update(WATCHDOG_ID_0, WATCH_DOG_TIMEOUT_TH);
// #endif
		if( ((judge_case_data&0xff) !=g_use_case) || ( (judge_case_data>>16) != g_trans_type ) ) {
			//cisdp_sensor_stop();
			model_change();
#ifdef CPU_24MHZ_VERSION
			SetPSPDNoVid_24M();//
#else
			SetPSPDNoVid();
#endif

		}


		//TODO: check if register changed by pc tool

		cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);

#if FRAME_CHECK_DEBUG
			if(g_spi_master_initial_status == 0) {
				if(hx_drv_spi_mst_open_speed(SPI_SEN_PIC_CLK) != 0)
				{
					dbg_printf(DBG_LESS_INFO, "DEBUG SPI master init fail\r\n");
					sensordplib_retrigger_capture();
					return ;
				}
				g_spi_master_initial_status = 1;
			}
			#ifdef UART_SEND_ALOGO_RESEULT

				hx_drv_swreg_aon_get_appused1(&judge_case_data);
				g_trans_type = (judge_case_data>>16);
				if( g_trans_type == 0 )// transfer type is (UART) 
				{

				}
				else if( g_trans_type == 1 || g_trans_type == 2)// transfer type is (SPI) or (UART & SPI) 
				{
					read_status = hx_drv_spi_mst_protocol_write_sp(jpeg_addr, jpeg_sz, DATA_TYPE_JPG);
				}
			#else 
				read_status = hx_drv_spi_mst_protocol_write_sp(jpeg_addr, jpeg_sz, DATA_TYPE_JPG);
			#endif
			#if DBG_APP_LOG
					dbg_printf(DBG_LESS_INFO, "write frame result %d, data size=%d,addr=0x%x\n",read_status,
							jpeg_sz,jpeg_addr);
			#endif
#endif
#ifdef EN_ALGO
#ifdef UART_SEND_ALOGO_RESEULT

	hx_drv_swreg_aon_get_appused1(&judge_case_data);
	g_trans_type = (judge_case_data>>16);
	if( g_trans_type == 0 )// transfer type is (UART) 
	{
		cv_yolo11n_ob_run(&algoresult_yolo11n_ob);
        update_i2c_detection_state_from_algo_result();
	}
	else if( g_trans_type == 1 || g_trans_type == 2)// transfer type is (SPI) or (UART & SPI) 
	{
		#if TOTAL_STEP_TICK
				uint32_t systick_1, systick_2;
				uint32_t loop_cnt_1, loop_cnt_2;
				SystemGetTick(&systick_1, &loop_cnt_1);
		#endif
			cv_yolo11n_ob_run(&algoresult_yolo11n_ob);
            update_i2c_detection_state_from_algo_result();

		#if TOTAL_STEP_TICK						
				SystemGetTick(&systick_2, &loop_cnt_2);
			#if TOTAL_STEP_TICK_DBG_LOG
				xprintf("Tick for TOTAL YOLO11N OB:[%d]\r\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));					
			#endif	

			if(g_trans_type == 1)//USE SPI
			{
				algoresult_yolo11n_ob.algo_tick = (loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2);		
			} 		
		#endif

		#if FRAME_CHECK_DEBUG
			hx_drv_spi_mst_protocol_write_sp((uint32_t)&algoresult_yolo11n_ob, sizeof(struct_yolov8_ob_algoResult), DATA_TYPE_META_YOLOV8_OB_DATA);
		#endif
	}
#else 
	#if TOTAL_STEP_TICK
			uint32_t systick_1, systick_2;
			uint32_t loop_cnt_1, loop_cnt_2;
			SystemGetTick(&systick_1, &loop_cnt_1);
	#endif

			cv_yolo11n_ob_run(&algoresult_yolo11n_ob);
            update_i2c_detection_state_from_algo_result();
	#if TOTAL_STEP_TICK						
			SystemGetTick(&systick_2, &loop_cnt_2);
		#if TOTAL_STEP_TICK_DBG_LOG
			xprintf("Tick for TOTAL YOLO11N OB:[%d]\r\n",(loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2));				
		#endif	
		algoresult_yolo11n_ob.algo_tick = (loop_cnt_2-loop_cnt_1)*CPU_CLK+(systick_1-systick_2);				
	#endif

	#if FRAME_CHECK_DEBUG
			hx_drv_spi_mst_protocol_write_sp((uint32_t)&algoresult_yolo11n_ob, sizeof(struct_yolov8_ob_algoResult), DATA_TYPE_META_YOLOV8_OB_DATA);
	#endif

#endif
		//clear_alg_hp_rsult
		for (int i = 0; i < MAX_TRACKED_YOLOV8_ALGO_RES; ++i) 
		{
			algoresult_yolo11n_ob.obr[i].bbox.x = 0;
			algoresult_yolo11n_ob.obr[i].bbox.y = 0;
			algoresult_yolo11n_ob.obr[i].bbox.width = 0;
			algoresult_yolo11n_ob.obr[i].bbox.height = 0;
			algoresult_yolo11n_ob.obr[i].confidence = 0;
			algoresult_yolo11n_ob.obr[i].class_idx = 0;
		}
#endif
		//recapture image
		//comment here, we will re-trigger at cv run
		//sensordplib_retrigger_capture();
	}

	if(g_md_detect == 1)
	{
		g_md_detect = 0;
	}

	if(g_inp1bitparer_abnormal == 1 || g_wdt1_timeout == 1 || g_wdt2_timeout == 1 || g_wdt3_timeout == 1
			|| g_cdm_fifoerror == 1 || g_xdma_abnormal == 1 || g_hxautoi2c_error == 1)
	{
		cisdp_sensor_stop();
	}
}

void app_start_state(APP_STATE_E state)
{
	if(cisdp_sensor_init() < 0)
	{
		xprintf("\r\nCIS Init fail\r\n");
#ifdef VST_UART_STATE_TX
        uart_report_boot_error_forever(3, 1);
#endif
		APP_BLOCK_FUNC();
	}

	dp_var_int();
#ifdef UART_SEND_ALOGO_RESEULT

	hx_drv_swreg_aon_get_appused1(&judge_case_data);
	g_trans_type = (judge_case_data>>16);
	if( g_trans_type == 0 || g_trans_type == 2)// transfer type is (UART) or (UART & SPI) 
	{
		if(state == APP_STATE_ALLON_YOLO11N_OB)
		{
			if(cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dp_app_cv_yolo11n_ob_eventhdl_cb, 4, APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X) < 0)
			{
				xprintf("\r\nDATAPATH Init fail\r\n");
#ifdef VST_UART_STATE_TX
                uart_report_boot_error_forever(3, 2);
#endif
				APP_BLOCK_FUNC();
			}
		}
	}
	else if ( g_trans_type == 1 ) // only use SPI 
	{
		if(state == APP_STATE_ALLON_YOLO11N_OB)
		{
			if(cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dp_app_cv_yolo11n_ob_eventhdl_cb, 4, APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X) < 0)
			{
				xprintf("\r\nDATAPATH Init fail\r\n");
#ifdef VST_UART_STATE_TX
                uart_report_boot_error_forever(3, 2);
#endif
				APP_BLOCK_FUNC();
			}
		}
	}
#else
	if(state == APP_STATE_ALLON_YOLO11N_OB)
	{
		if(cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dp_app_cv_yolo11n_ob_eventhdl_cb, 4, APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X) < 0)
        {
        	xprintf("\r\nDATAPATH Init fail\r\n");
#ifdef VST_UART_STATE_TX
            uart_report_boot_error_forever(3, 2);
#endif
        	APP_BLOCK_FUNC();
        }
	}
#endif
	event_handler_init();

    cisdp_sensor_start();

   	event_handler_start();
}

void model_change() {
	//stop event
	event_handler_stop();
	event_handler_deinit();
	cisdp_sensor_stop();
#ifdef EN_ALGO
	if(g_use_case==0) {

		cv_yolo11n_ob_deinit();
	}
#endif
	//hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD, true);
#if 0
	//restart all thing in main
	//check current use case
	g_use_case = EPII_get_memory(0x5610F02c);


	mm_set_initial(BOOT2NDLOADER_BASE, 0x00200000-(BOOT2NDLOADER_BASE-0x34000000));

	if(g_use_case == 0) {
		xprintf("Face Detection Face Land mark \n");
		//cv_fd_fl_init(bool security_enable, bool privilege_enable, uint32_t fd_model_addr, uint32_t fl_model_addr, uint32_t el_model_addr);
		cv_fd_fl_init(true, true, FACE_DECTECT_FLASH_ADDR, FACE_LANDMARK_FLASH_ADDR, EYE_LANDMARK_FLASH_ADDR);
		app_start_state(APP_STATE_ALLON_FD_FL);
	}
	else if(g_use_case == 1) {
		//pose landmark with movenet
		xprintf("pose landmark with movenet\n");
		cv_pl_init(true, true, HUMAN_DETECTION_FLASH_ADDR, POSE_LANDMARKS_MOVENET_FLASH_ADDR);
		app_start_state(APP_STATE_ALLON_PL);
	}
	else if(g_use_case == 2) {
		//pose landmark with hrnet
		xprintf("pose landmark with hrnet\n");
		cv_pl_init(true, true, HUMAN_DETECTION_FLASH_ADDR, POSE_LANDMARKS_HRNET_FLASH_ADDR);
		app_start_state(APP_STATE_ALLON_PL);
	}
#endif
}
/*******************************************************************************
 * Code
 ******************************************************************************/
/*!
 * @brief Main function
 */
int tflm_yolo11_od_app(void) {

	uint32_t wakeup_event;
	uint32_t wakeup_event1;
	uint32_t freq=0;

	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT, &wakeup_event);
	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT1, &wakeup_event1);

    hx_drv_swreg_aon_get_pllfreq(&freq);
    xprintf("wakeup_event=0x%x,WakeupEvt1=0x%x, freq=%d\n", wakeup_event, wakeup_event1, freq);

    pinmux_init();
#ifdef VST_I2C_STATE_TX
    i2c_slave_init_for_detection_state();
#endif

    //SCB_DisableICache();
    //SCB_DisableDCache();
    //cold boot
	if((wakeup_event == PMU_WAKEUP_NONE) && (wakeup_event1 == PMU_WAKEUPEVENT1_NONE)) {
	}
	else {
		hx_lib_pm_ctrl_fromPMUtoCPU(NULL);
	}

	hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q);
    //hx_lib_spi_eeprom_open_speed(USE_DW_SPI_MST_Q, 6000000);

	hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true);

	//
// #ifdef WATCHDOG_VERSION

// 	//watch dog start
// 	WATCHDOG_CFG_T wdg_cfg;
// 	wdg_cfg.period = WATCH_DOG_TIMEOUT_TH;
// 	wdg_cfg.ctrl = WATCHDOG_CTRL_CPU;
// 	wdg_cfg.state = WATCHDOG_STATE_DC;
// 	wdg_cfg.type = WATCHDOG_RESET;//wewweWATCHDOG_INT;
// 	hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg , WDG_Reset_ISR_CB);
// 	xprintf("hx_drv_watchdog_start\n");
// #endif

	//check current use case
	// //check current use case
	//judge which case

	hx_drv_swreg_aon_get_appused1(&judge_case_data);
	//transfer type
	g_trans_type = (judge_case_data>>16);
	//model case 
	g_use_case = (judge_case_data&0xff);


#ifndef CPU_24MHZ_VERSION
	xprintf("ori_clk src info, 0x56100030=%x\n",EPII_get_memory(0x56100030));
	xprintf("ori_clk src info, 0x56100034=%x\n",EPII_get_memory(0x56100034));
	xprintf("ori_clk src info, 0x56100038=%x\n",EPII_get_memory(0x56100038));

	EPII_set_memory(0x56100030,0x4037);
	EPII_set_memory(0x56100034,0x0);
	EPII_set_memory(0x56100038,0xc1b8);

	xprintf("clk src info, 0x56100030=%x\n",EPII_get_memory(0x56100030));
	xprintf("clk src info, 0x56100034=%x\n",EPII_get_memory(0x56100034));
	xprintf("clk src info, 0x56100038=%x\n",EPII_get_memory(0x56100038));
#endif
	// mm_set_initial(BOOT2NDLOADER_BASE, 0x00200000-(BOOT2NDLOADER_BASE-0x34000000));
#ifdef __GNU__
	xprintf("__GNUC \n");
	extern char __mm_start_addr__;
	xprintf("__mm_start_addr__ address: %x\r\n",&__mm_start_addr__);
	mm_set_initial((int)(&__mm_start_addr__), 0x00200000-((int)(&__mm_start_addr__)-0x34000000));
#else
	static uint8_t mm_start_addr __attribute__((section(".bss.mm_start_addr")));
	xprintf("mm_start_addr address: %x \r\n",&mm_start_addr);
	mm_set_initial((int)(&mm_start_addr), 0x00200000-((int)(&mm_start_addr)-0x34000000));
#endif
	if(g_use_case == 0) {
		xprintf("YOLO11n object detection\n");
#ifdef EN_ALGO
		cv_yolo11n_ob_init(true, true, YOLO11_OBJECT_DETECTION_FLASH_ADDR);
#endif
	    app_start_state(APP_STATE_ALLON_YOLO11N_OB);
	}
	return 0;
}
