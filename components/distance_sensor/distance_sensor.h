#ifndef __DISTANCE_SENSOR_H__
#define __DISTANCE_SENSOR_H__


#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>

#define ESP_ERR_ULTRASONIC_PING_TIMEOUT 0x201
#define ESP_ERR_ULTRASONIC_ECHO_TIMEOUT 0x202

typedef struct
{
    gpio_num_t trigger_pin; 
    gpio_num_t echo_pin;    
} distance_sensor_t;


esp_err_t distance_init(const distance_sensor_t *dev);
esp_err_t get_distance(const distance_sensor_t *dev, float *distance);
esp_err_t get_distance_average(const distance_sensor_t *dev, float *distance);
esp_err_t distance_measure_cm(const distance_sensor_t *dev, float *distance);
void convert_time_to_cm(volatile int64_t time, float *distance);
bool timeout_expired(int64_t start, int64_t dur);
float get_temperature_cm();

#endif // __DISTANCE_SENSOR_H__