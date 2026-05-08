#include "stepper.h"

#include <string.h>

/* TB6612FNG motor shield pin mapping imported from external/t-sim-motor-shield. */
#ifndef D2_GPIO_CFG
  #define D2_GPIO_CFG 9
#endif
#ifndef D3_GPIO_CFG
  #define D3_GPIO_CFG 10
#endif
#ifndef D4_GPIO_CFG
  #define D4_GPIO_CFG 11
#endif
#ifndef D5_GPIO_CFG
  #define D5_GPIO_CFG 12
#endif
#ifndef D6_GPIO_CFG
  #define D6_GPIO_CFG 13
#endif
#ifndef D7_GPIO_CFG
  #define D7_GPIO_CFG 14
#endif
#ifndef D0_GPIO_CFG
  #define D0_GPIO_CFG 3
#endif

static const int PIN_RIGHT_LED = (int)D0_GPIO_CFG;
static const int PIN_PWMA = (int)D2_GPIO_CFG;
static const int PIN_AIN2 = (int)D3_GPIO_CFG;
static const int PIN_AIN1 = (int)D4_GPIO_CFG;
static const int PIN_BIN2 = (int)D5_GPIO_CFG;
static const int PIN_BIN1 = (int)D6_GPIO_CFG;
static const int PIN_PWMB = (int)D7_GPIO_CFG;

static const int PWM_FREQ_HZ = 20000;
static const int PWM_BITS = 8;
static const int PWM_CH_A = 0;
static const int PWM_CH_B = 1;

static StepperConfig g_stepper_config;
static bool g_stepper_ready = false;

static const int8_t FULLSTEP[4][2] = {
    { +1, +1 },
    { -1, +1 },
    { -1, -1 },
    { +1, -1 },
};

static inline void coast_a()
{
    digitalWrite(PIN_AIN1, LOW);
    digitalWrite(PIN_AIN2, LOW);
}

static inline void coast_b()
{
    digitalWrite(PIN_BIN1, LOW);
    digitalWrite(PIN_BIN2, LOW);
}

static inline void stepper_coast()
{
    coast_a();
    coast_b();
}

static inline void set_right_led(bool active)
{
    digitalWrite(PIN_RIGHT_LED, active ? HIGH : LOW);
}

static inline void drive_a_pol(int pol)
{
    if (pol > 0) {
        digitalWrite(PIN_AIN1, HIGH);
        digitalWrite(PIN_AIN2, LOW);
    } else if (pol < 0) {
        digitalWrite(PIN_AIN1, LOW);
        digitalWrite(PIN_AIN2, HIGH);
    } else {
        coast_a();
    }
}

static inline void drive_b_pol(int pol)
{
    if (pol > 0) {
        digitalWrite(PIN_BIN1, HIGH);
        digitalWrite(PIN_BIN2, LOW);
    } else if (pol < 0) {
        digitalWrite(PIN_BIN1, LOW);
        digitalWrite(PIN_BIN2, HIGH);
    } else {
        coast_b();
    }
}

static inline void apply_phase(uint8_t idx)
{
    idx &= 3;
    drive_a_pol(FULLSTEP[idx][0]);
    drive_b_pol(FULLSTEP[idx][1]);
}

static inline void wait_until(uint32_t target_us)
{
    while ((int32_t)(micros() - target_us) < 0) {
        delay(0);
    }
}

static bool start_direction_is_clockwise()
{
    return strcmp(g_stepper_config.start_direction, "anti-clockwise") != 0;
}

static void rotate_configured(bool forward, bool coast_after)
{
    if (!g_stepper_ready)
        return;

    uint32_t speed = g_stepper_config.speed_steps_per_second;
    uint32_t steps_per_rev = g_stepper_config.steps_per_revolution;
    uint32_t degrees = g_stepper_config.rotation_degrees;

    if (speed == 0)
        speed = 200;
    if (steps_per_rev == 0)
        steps_per_rev = 2048;

    uint32_t steps = (steps_per_rev * degrees + 180) / 360;
    if (steps == 0)
        steps = 1;

    uint32_t period_us = 1000000UL / speed;
    uint8_t phase = forward ? 0 : 3;
    apply_phase(phase);

    Serial.printf("STEPPER: rotate %s start mode=stepper steps=%lu degrees=%lu speed=%lu period_us=%lu coast_after=%s\n",
                  forward ? "forward" : "reverse",
                  (unsigned long)steps,
                  (unsigned long)degrees,
                  (unsigned long)speed,
                  (unsigned long)period_us,
                  coast_after ? "YES" : "NO");

    uint32_t next_us = micros() + period_us;
    for (uint32_t i = 0; i < steps; i++) {
        wait_until(next_us);
        next_us += period_us;
        phase = forward ? ((phase + 1) & 3) : ((phase + 3) & 3);
        apply_phase(phase);
    }

    if (coast_after)
        stepper_coast();

    Serial.println("STEPPER: rotate done");
}

void stepper_init(const StepperConfig &config)
{
    g_stepper_config = config;

    pinMode(PIN_AIN1, OUTPUT);
    pinMode(PIN_AIN2, OUTPUT);
    pinMode(PIN_BIN1, OUTPUT);
    pinMode(PIN_BIN2, OUTPUT);
    pinMode(PIN_PWMA, OUTPUT);
    pinMode(PIN_PWMB, OUTPUT);
    pinMode(PIN_RIGHT_LED, OUTPUT);
    set_right_led(false);

    ledcSetup(PWM_CH_A, PWM_FREQ_HZ, PWM_BITS);
    ledcSetup(PWM_CH_B, PWM_FREQ_HZ, PWM_BITS);
    ledcAttachPin(PIN_PWMA, PWM_CH_A);
    ledcAttachPin(PIN_PWMB, PWM_CH_B);
    ledcWrite(PWM_CH_A, 255);
    ledcWrite(PWM_CH_B, 255);

    stepper_coast();
    g_stepper_ready = true;

    Serial.printf("STEPPER: initialized speed=%u steps/s rotation=%u deg reverse_wait=%u ms steps_per_rev=%u start_direction=%s right_led_gpio=%d pins PWMA=%d AIN2=%d AIN1=%d BIN2=%d BIN1=%d PWMB=%d\n",
                  g_stepper_config.speed_steps_per_second,
                  g_stepper_config.rotation_degrees,
                  g_stepper_config.reverse_wait_ms,
                  g_stepper_config.steps_per_revolution,
                  g_stepper_config.start_direction,
                  PIN_RIGHT_LED,
                  PIN_PWMA,
                  PIN_AIN2,
                  PIN_AIN1,
                  PIN_BIN2,
                  PIN_BIN1,
                  PIN_PWMB);
}

void stepper_rotate_configured(bool forward)
{
    rotate_configured(forward, true);
}

bool stepper_run_configured_cycle()
{
    if (!g_stepper_ready)
        return false;

    bool start_clockwise = start_direction_is_clockwise();
    Serial.printf("STEPPER: actuator cycle start_direction=%s\n",
                  start_clockwise ? "clockwise" : "anti-clockwise");
    Serial.printf("STEPPER: right LED ON gpio=%d\n", PIN_RIGHT_LED);
    set_right_led(true);
    rotate_configured(start_clockwise, false);
    uint32_t wait_ms = g_stepper_config.reverse_wait_ms == 0 ? 1000 : g_stepper_config.reverse_wait_ms;
    Serial.printf("STEPPER: holding start phase %lu ms before return\n", (unsigned long)wait_ms);
    delay(wait_ms);
    Serial.println("STEPPER: actuator cycle return");
    rotate_configured(!start_clockwise, true);
    set_right_led(false);
    Serial.printf("STEPPER: right LED OFF gpio=%d\n", PIN_RIGHT_LED);
    Serial.println("STEPPER: actuator cycle done");
    return true;
}

void stepper_run_post_test_cycle()
{
    if (!g_stepper_ready)
        return;

    Serial.println("STEPPER: POST test cycle begin");
    bool ok = stepper_run_configured_cycle();
    Serial.printf("STEPPER: POST test cycle %s\n", ok ? "done" : "skipped");
}
