/*
 * button_adc.c — Resistor-ladder button driver for ESP32-S3-EYE.
 *
 * Voltage thresholds (ADC full-scale 3300mV at 12-bit):
 *   UP   ~350mV  → < 600mV   (raw < 744)
 *   DOWN ~800mV  → 600-1400  (raw 744–1736)
 *   PLAY ~1920mV → 1400-2400 (raw 1736–2979)
 *   MENU ~2800mV → 2400-3100 (raw 2979–3847)
 *   NONE ~3300mV → > 3100    (raw > 3847)
 *
 * (raw = mV * 4095 / 3300, approximate — actual attenuation is 11dB)
 */

#include "button_adc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "button_adc";

#define BTN_ADC_UNIT     ADC_UNIT_1
#define BTN_ADC_CHANNEL  ADC_CHANNEL_0   /* GPIO1 */
#define BTN_ADC_ATTEN    ADC_ATTEN_DB_12 /* 0-3.3V range */
#define BTN_ADC_BITWIDTH ADC_BITWIDTH_12

/* Thresholds in millivolts */
#define MV_UP_MAX    600
#define MV_DOWN_MAX  1400
#define MV_PLAY_MAX  2400
#define MV_MENU_MAX  3100

#define POLL_MS        20
#define DEBOUNCE_COUNT  3    /* 3 × 20ms = 60ms debounce */
#define LONG_PRESS_MS  1000

#define QUEUE_DEPTH  8

static adc_oneshot_unit_handle_t g_adc_handle;
static adc_cali_handle_t         g_cali_handle;
static bool                      g_cali_ok;
static QueueHandle_t             g_queue;

/* ── Voltage → button ID ────────────────────────────────────────────────── */
static button_id_t mv_to_button(int mv)
{
    if (mv < MV_UP_MAX)   return BTN_UP;
    if (mv < MV_DOWN_MAX) return BTN_DOWN;
    if (mv < MV_PLAY_MAX) return BTN_PLAY;
    if (mv < MV_MENU_MAX) return BTN_MENU;
    return BTN_NONE;
}

/* ── Poll task ──────────────────────────────────────────────────────────── */
static void poll_task(void *arg)
{
    button_id_t debounced  = BTN_NONE;   /* confirmed current state */
    button_id_t candidate  = BTN_NONE;   /* state being debounced */
    int         consec     = 0;          /* consecutive same readings */
    int64_t     press_tick = 0;          /* tick when press confirmed */
    bool        long_fired = false;      /* long-press event already sent */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        /* Read ADC */
        int raw = 0;
        adc_oneshot_read(g_adc_handle, BTN_ADC_CHANNEL, &raw);

        int mv = 0;
        if (g_cali_ok) {
            adc_cali_raw_to_voltage(g_cali_handle, raw, &mv);
        } else {
            /* Fallback: linear approximation, 12-bit, 3300mV full scale */
            mv = (int)((int64_t)raw * 3300 / 4095);
        }

        button_id_t sampled = mv_to_button(mv);

        /* Debounce: accumulate consecutive identical samples */
        if (sampled == candidate) {
            consec++;
        } else {
            candidate = sampled;
            consec    = 1;
        }

        if (consec < DEBOUNCE_COUNT) continue; /* not yet stable */

        /* State transition */
        if (sampled != debounced) {
            button_id_t prev = debounced;
            debounced = sampled;

            if (sampled != BTN_NONE) {
                /* Press confirmed */
                press_tick  = xTaskGetTickCount() * portTICK_PERIOD_MS;
                long_fired  = false;
            } else {
                /* Release: classify as short if long hasn't fired */
                if (prev != BTN_NONE && !long_fired) {
                    button_event_t evt = { .id = prev, .type = BTN_EVT_SHORT_PRESS };
                    xQueueSend(g_queue, &evt, 0);
                    ESP_LOGD(TAG, "btn %d short", prev);
                }
            }
        }

        /* Check for long press while still held */
        if (debounced != BTN_NONE && !long_fired) {
            int64_t held_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS - press_tick;
            if (held_ms >= LONG_PRESS_MS) {
                button_event_t evt = { .id = debounced, .type = BTN_EVT_LONG_PRESS };
                xQueueSend(g_queue, &evt, 0);
                long_fired = true;
                ESP_LOGD(TAG, "btn %d long", debounced);
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t button_adc_init(void)
{
    g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(button_event_t));
    if (!g_queue) return ESP_ERR_NO_MEM;

    /* ADC oneshot init */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BTN_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BTN_ADC_ATTEN,
        .bitwidth = BTN_ADC_BITWIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, BTN_ADC_CHANNEL, &chan_cfg));

    /* Calibration (optional — fallback to linear if unavailable) */
    g_cali_ok = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BTN_ADC_UNIT,
        .chan     = BTN_ADC_CHANNEL,
        .atten   = BTN_ADC_ATTEN,
        .bitwidth = BTN_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_cali_handle) == ESP_OK) {
        g_cali_ok = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = BTN_ADC_UNIT,
        .atten   = BTN_ADC_ATTEN,
        .bitwidth = BTN_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &g_cali_handle) == ESP_OK) {
        g_cali_ok = true;
    }
#endif
    ESP_LOGI(TAG, "ADC calibration: %s", g_cali_ok ? "OK" : "fallback");

    xTaskCreate(poll_task, "btn_adc", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

QueueHandle_t button_adc_get_queue(void)
{
    return g_queue;
}
