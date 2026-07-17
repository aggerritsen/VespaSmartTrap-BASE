#pragma once

#include <Arduino.h>
#include "sdcard.h"

void stepper_init(const StepperConfig &config);
void stepper_rotate_configured(bool forward);
bool stepper_run_configured_cycle();
void stepper_run_post_test_cycle();
void stepper_run_post_start_led_signal();
void stepper_run_status_led_post_test();
void stepper_set_status_led(bool active);
