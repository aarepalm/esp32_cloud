/*
 * button_adc.h — Resistor-ladder button driver for ESP32-S3-EYE.
 *
 * All four buttons share ADC1 channel 0 (GPIO1).
 * The driver polls every 20ms, debounces over 3 consecutive reads (60ms),
 * and classifies events as short press (< 1000ms) or long press (≥ 1000ms).
 *
 * Usage:
 *   button_adc_init();
 *   QueueHandle_t q = button_adc_get_queue();
 *   button_event_t evt;
 *   while (xQueueReceive(q, &evt, portMAX_DELAY)) { ... }
 */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_UP,
    BTN_DOWN,
    BTN_PLAY,
    BTN_MENU,
} button_id_t;

typedef enum {
    BTN_EVT_SHORT_PRESS = 0,
    BTN_EVT_LONG_PRESS,
} button_event_type_t;

typedef struct {
    button_id_t         id;
    button_event_type_t type;
} button_event_t;

/**
 * @brief Initialise ADC and start the poll task.
 */
esp_err_t button_adc_init(void);

/**
 * @brief Return the event queue (depth 8). Read button_event_t items from it.
 */
QueueHandle_t button_adc_get_queue(void);

#ifdef __cplusplus
}
#endif
