#include "distance_sensor.h"

#include <esp_timer.h>
#include <inttypes.h>
#include <rom/ets_sys.h>

#define NUM_SENSOR_ERROR_RETRIES 10
#define NUM_SENSOR_AVERAGE 5.0f
#define TRIGGER_LOW_DELAY 4
#define TRIGGER_HIGH_DELAY 10
#define PING_TIMEOUT 600000
#define ROUNDTRIP_CM 58.2377
#define MAX_ECHO_TIME 250 * ROUNDTRIP_CM // 10cm max distance
#define MAX_DISTANCE 7.0f // The maximum distance accepted for a 
static const char *TAG = "DISTANCE_SENSOR";

esp_err_t distance_init(const distance_sensor_t *dev) {
    gpio_set_direction(dev->trigger_pin, GPIO_MODE_OUTPUT);
    gpio_set_direction(dev->echo_pin, GPIO_MODE_INPUT);
    return gpio_set_level(dev->trigger_pin, 0);
}

esp_err_t distance_measure_cm(const distance_sensor_t *dev, float *distance) {
    gpio_set_level(dev->trigger_pin, 0);
    ets_delay_us(TRIGGER_LOW_DELAY);
    gpio_set_level(dev->trigger_pin, 1);
    ets_delay_us(TRIGGER_HIGH_DELAY);
    gpio_set_level(dev->trigger_pin, 0);

    int64_t start = esp_timer_get_time();

    while (!gpio_get_level(dev->echo_pin)) {
        if (timeout_expired(start, PING_TIMEOUT)) {
            return ESP_ERR_ULTRASONIC_PING_TIMEOUT;
        }
    }

    volatile int64_t echo_start = esp_timer_get_time();
    volatile int64_t time = echo_start;

    // Wait for pin to go low again
    while (gpio_get_level(dev->echo_pin)) {
        time = esp_timer_get_time();
        if (timeout_expired(start, MAX_ECHO_TIME)) {
            return ESP_ERR_ULTRASONIC_ECHO_TIMEOUT;
        }
    }

    int64_t time_delta = time - echo_start;
    convert_time_to_cm(time_delta, distance);
    return ESP_OK;
}

void convert_time_to_cm(volatile int64_t time, float *distance) {
    float calculated_distance = (float)time / ROUNDTRIP_CM;
    *distance = calculated_distance;
}

bool timeout_expired(int64_t time, int64_t dur) {
    int64_t curr_dur = esp_timer_get_time() - time;
    return curr_dur >= dur;
}

float get_temperature_cm() {
    return 0.0f;
}

esp_err_t get_distance(const distance_sensor_t *dev, float *distance) {
    esp_err_t res = distance_measure_cm(dev, distance);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Sensor Error %d: ", res);
        switch (res) {
        case ESP_ERR_ULTRASONIC_PING_TIMEOUT:
            ESP_LOGE(TAG, "Sensor PING error - sensor is likely too close to object");
            break;
        case ESP_ERR_ULTRASONIC_ECHO_TIMEOUT:
            ESP_LOGE(TAG, "Sensor ECHO error - sensor is likely too far from object");
            break;
        default:
            ESP_LOGE(TAG, "UNKNOWN sensor error - %s", esp_err_to_name(res));
            break;
        }
        int num_errors = 0;
        while (num_errors < NUM_SENSOR_ERROR_RETRIES && res != ESP_OK) { // attempt to get sensor reading
            num_errors++;
            ets_delay_us(60000);
            res = distance_measure_cm(dev, distance);
        }
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "FAILED TO GET SENSOR READING AFTER %i reties", NUM_SENSOR_ERROR_RETRIES);
            return ESP_FAIL;
        } else {
            return ESP_OK;
        }
    }
    return ESP_OK;
}

esp_err_t get_distance_average(const distance_sensor_t *dev, float *distance) {
    float sum = 0;
    for (int i = 0; i < NUM_SENSOR_AVERAGE; i++) {
        ets_delay_us(60000);
        float measurement;
        esp_err_t err = get_distance(dev, &measurement);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot get average - sensor fail");
            return ESP_FAIL;
        }
        sum += measurement;
    }
    *distance = sum / NUM_SENSOR_AVERAGE;
    return ESP_OK;
}
