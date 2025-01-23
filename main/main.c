#include <cJSON.h>
#include <distance_sensor.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <protocol_examples_common.h>
#include <protocol_examples_utils.h>

// TODO: The turn pump on/off buttons should be reduced to just one button that's the opposite action of what the current state is

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN (64)
#define TRIGGER_GPIO GPIO_NUM_0
#define ECHO_GPIO GPIO_NUM_1
#define PUMP_PIN GPIO_NUM_4
#define NUM_BELOW_TRIGGER 3     // the number of sensor readings that must be below the trigger value for it to count
#define MAX_TOPUP_TIME 15000000 // the maximum topup time is 15s. This is to prevent a case where a sensor issue may cause the topup to never end.
#define TRIGGER_REACHED "Trigger level reached"
#define PUMP_TIMEOUT "The pump on time limit was reached"
#define SENSOR_ERROR "Sensor error"
#define TOPUP_NOT_NEEDED "Topup not needed"

static distance_sensor_t sensor = {
    .trigger_pin = TRIGGER_GPIO,
    .echo_pin = ECHO_GPIO};

static nvs_handle_t my_handle;
static const char *TAG = "example";
static const char *TAG_STORAGE = "storage";
static const char *TAG_TIME = "time";
static const char *TAG_SERVER = "server";
static const char *TAG_PUMP = "pump";
static bool pump_state = false;
static bool doTopup = false;
static uint8_t daysOfTheWeek = 9; // stores which days of the week the system must trigger on with one bit per day. Bit 7 is not used. Bit 6 is Sunday and so on. Default is Monday and Thursday
static uint8_t triggerHour = 14;
static uint8_t triggerMinute = 30;
static float trigger_level = 3.0f;
static char last_trigger[30] = {0};
static char last_trigger_reason[40] = {0};
RTC_DATA_ATTR static int boot_count = 0;

static void obtain_time(void);
void topup_task();
void start_timer();

void pump_off() {
    ESP_LOGD(TAG_PUMP, "Turning pump off");
    gpio_set_level(PUMP_PIN, 0);
    pump_state = false;
}

void pump_on() {
    ESP_LOGD(TAG_PUMP, "Turning pump on");
    gpio_set_level(PUMP_PIN, 1);
    pump_state = true;
}

static float get_current_water_level(float *distance) {
    return get_distance_average(&sensor, distance);
}

static float get_trigger_level() {
    static bool executed = false;

    if (executed) {
        return trigger_level;
    } else {
        int32_t trigger_level_mm;
        esp_err_t err = nvs_get_i32(my_handle, "trigger_level", &trigger_level_mm);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG_STORAGE, "No saved trigger level found, using safe value of %.2fcm.", trigger_level);
        } else if (err == ESP_OK) { // stored value found, using it
            trigger_level = trigger_level_mm / 1000.0f;
            ESP_LOGI(TAG_STORAGE, "Found saved trigger level: %f cm", trigger_level);
            executed = true;
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s)", esp_err_to_name(err));
        }
        return trigger_level;
    }
}

static void set_trigger_level(float i) {
    int32_t trigger_level_mm = i * 1000;
    ESP_ERROR_CHECK(nvs_set_i32(my_handle, "trigger_level", trigger_level_mm));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    ESP_LOGI(TAG_STORAGE, "Trigger level saved.");
    trigger_level = i;
}

static void set_trigger_hour(uint8_t value) {
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "trigger_hour", value));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    triggerHour = value;
}

static uint8_t get_trigger_hour() {
    static bool executed = false;

    // Store value in memory, don't need to fetch from flash each time
    if (executed) {
        return triggerHour;
    } else {
        esp_err_t err = nvs_get_u8(my_handle, "trigger_hour", &triggerHour);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG_STORAGE, "No saved trigger hour found, using default value of: %i.", triggerHour);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG_STORAGE, "Found saved trigger hour: %i", triggerHour);
            executed = true; // Only set true on a successfull NVS read - will want to retry in other circumstances.
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s)for trigger_hour.\n Using default value of: %i.", esp_err_to_name(err), triggerHour);
        }
    }
    return triggerHour;
}

static void set_trigger_minute(uint8_t value) {
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "trigger_minute", value));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    triggerMinute = value;
}

static uint8_t get_trigger_minute() {
    static bool executed = false;

    // Store value in memory, don't need to fetch from flash each time
    if (executed) {
        return triggerMinute;
    } else {
        esp_err_t err = nvs_get_u8(my_handle, "trigger_minute", &triggerMinute);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG_STORAGE, "No saved trigger minute found, using default value of: %i.", triggerMinute);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG_STORAGE, "Found saved trigger minute: %i", triggerMinute);
            executed = true; // Only set true on a successfull NVS read - will want to retry in other circumstances.
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s) for trigger_minute.\n Using default value of: %i.", esp_err_to_name(err), triggerMinute);
        }
    }
    return triggerMinute;
}

static void set_trigger_days(uint8_t value) {
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "trigger_days", value));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    daysOfTheWeek = value;
}

static uint8_t get_trigger_days() {
    static bool executed = false;

    // Store value in memory, don't need to fetch from flash each time
    if (executed) {
        return daysOfTheWeek;
    } else {
        esp_err_t err = nvs_get_u8(my_handle, "trigger_days", &daysOfTheWeek);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG_STORAGE, "No saved trigger days found, using default value of: %i (Mon, Thurs).", daysOfTheWeek);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG_STORAGE, "Found saved trigger days: %i", daysOfTheWeek);
            executed = true; // Only set true on a successfull NVS read - will want to retry in other circumstances.
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s) for trigger_days.\n Using default value of: %i (Mon, Thurs).", esp_err_to_name(err), daysOfTheWeek);
        }
    }
    return daysOfTheWeek;
}

static void set_last_trigger(const char *time_str) {
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "last_trigger", time_str));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    strncpy(last_trigger, time_str, sizeof(last_trigger));
}

static void get_last_trigger() {
    static bool executed = false;

    // Value is already in memory
    if (executed) {
        return;
    } else {
        size_t length = sizeof(last_trigger);
        esp_err_t err = nvs_get_str(my_handle, "last_trigger", last_trigger, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG_STORAGE, "No saved last trigger time found, using default value of: No data");
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG_STORAGE, "Found saved last trigger time: %s", last_trigger);
            executed = true; // Only set true on a successfull NVS read - will want to retry in other circumstances.
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s) for last_trigger.\n Using default value of: No data.", esp_err_to_name(err));
        }
    }
    return;
}

static void set_trigger_reason(const char *reason) {
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "trigger_reason", last_trigger_reason));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    strncpy(last_trigger_reason, reason, sizeof(last_trigger_reason));
}

static void get_trigger_reason() {
    static bool executed = false;

    // Value is already in memory
    if (executed) {
        return;
    } else {
        size_t length = sizeof(last_trigger_reason);
        esp_err_t err = nvs_get_str(my_handle, "trigger_reason", last_trigger_reason, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG_STORAGE, "No saved last trigger reason found, using default value of: %s", TRIGGER_REACHED);
        } else if (err == ESP_OK) {
            ESP_LOGI(TAG_STORAGE, "Found saved trigger_reason: %s", last_trigger_reason);
            executed = true; // Only set true on a successfull NVS read - will want to retry in other circumstances.
        } else {
            ESP_LOGE(TAG_STORAGE, "Error reading NVS (%s) for trigger_reason.\n Using default value of: %s", esp_err_to_name(err), TRIGGER_REACHED);
        }
    }
    return;
}

static bool get_pump_state() {
    return pump_state;
}

static void set_pump_state(bool state) {
    pump_state = state;
    ESP_ERROR_CHECK(gpio_set_level(PUMP_PIN, state));
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SERVER, "Handling root request");
    char *buf;
    size_t buf_len;

    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG_SERVER, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG_SERVER, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN], dec_param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN] = {0};
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "query1", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query1=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
            if (httpd_query_key_value(buf, "query3", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query3=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
            if (httpd_query_key_value(buf, "query2", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query2=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
        }
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char *resp_str = (const char *)req->user_ctx;
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = "<!DOCTYPE html>"
                "<html lang=\"en\">"
                "<head>"
                "    <meta charset=\"UTF-8\">"
                "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                "    <title>Aquarium Auto Top-Off</title>"
                "    <link rel=\"stylesheet\" href=\"/styles.css\">"
                "    <script src=\"/script.js\" defer></script>"
                "</head>"
                "<body>"
                "    <header>"
                "        <h1>Aquarium Auto Top-Off</h1>"
                "    </header>"
                "    <main>"
                "        <section>"
                "            <table>"
                "                <thead>"
                "                    <tr>"
                "                        <th>Parameter</th>"
                "                        <th>Value</th>"
                "                    </tr>"
                "                </thead>"
                "                <tbody>"
                "                    <tr>"
                "                        <td>Water Level</td>"
                "                        <td id=\"water-level\">-</td>"
                "                    </tr>"
                "                    <tr>"
                "                        <td>Pump State</td>"
                "                        <td id=\"pump-state\">-</td>"
                "                    </tr>"
                "                    <tr>"
                "                      <td>Current system time</td>"
                "                      <td id=\"current-system-time\">-</td>"
                "                    </tr>"
                "                    <tr>"
                "                      <td>Last trigger time</td>"
                "                      <td id=\"last-trigger-time\">-</td>"
                "                    </tr>"
                "                </tbody>"
                "            </table>"
                "        </section>"
                "        <section class=\"controls\">"
                "          <!-- Toggle Buttons -->"
                "          <div class=\"toggle-buttons\">"
                "              <button onclick=\"togglePump('on')\">Turn Pump ON</button>"
                "              <button onclick=\"togglePump('off')\">Turn Pump OFF</button>"
                "              <button onclick=\"topUp()\">Top-Off Now</button>"
                "          </div>"
                "      "
                "          <!-- Textbox Section -->"
                "          <div class=\"trigger-controls\">"
                "              <label for=\"trigger-input\">Set Trigger Level (cm):</label>"
                "              <input type=\"number\" id=\"trigger-input\" step=\"0.01\" min = \"0\">"
                "              <button onclick=\"setTriggerLevel()\">Set</button>"
                "          </div>"
                ""
                "          <div class=\"schedule-controls\">"
                "            <label for=\"time-input\">Set Trigger Time:</label>"
                "            <input type=\"time\" id=\"time-input\">"
                "        "
                "            <div class=\"days-toggle\">"
                "                <button type=\"button\" class=\"day-button\" data-day=\"1\">Mon</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"2\">Tue</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"3\">Wed</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"4\">Thu</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"5\">Fri</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"6\">Sat</button>"
                "                <button type=\"button\" class=\"day-button\" data-day=\"7\">Sun</button>"
                "            </div>"
                "        "
                "            <button onclick=\"setSchedule()\">Set Schedule</button>"
                "        </div>"
                "      </section>"
                "    </main>"
                "</body>"
                "</html>"};

esp_err_t css_get_handler(httpd_req_t *req) {
    const char *css =
        "body {"
        "    font-family: Arial, sans-serif;"
        "    margin: 0;"
        "    padding: 0;"
        "    background-color: #f4f4f9;"
        "    color: #333;"
        "}"
        ""
        "header {"
        "    background-color: #0077cc;"
        "    color: white;"
        "    text-align: center;"
        "    padding: 1rem;"
        "}"
        ""
        "main {"
        "    max-width: 600px;"
        "    margin: 2rem auto;"
        "    padding: 1rem;"
        "    background: #fff;"
        "    border-radius: 8px;"
        "    box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);"
        "}"
        ""
        "table {"
        "    width: 100%;"
        "    border-collapse: collapse;"
        "    margin-bottom: 1rem;"
        "}"
        ""
        "table th, table td {"
        "    border: 1px solid #ddd;"
        "    padding: 0.5rem;"
        "    text-align: center;"
        "}"
        ""
        "table th {"
        "    background-color: #0077cc;"
        "    color: white;"
        "}"
        ""
        ".controls {"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "    gap: 1rem; /* Spacing between sections */"
        "}"
        ""
        "/* Toggle Buttons Section */"
        ".toggle-buttons {"
        "    display: flex;"
        "    flex-wrap: wrap;"
        "    gap: 1rem; /* Spacing between the buttons */"
        "    justify-content: center; /* Center buttons horizontally */"
        "}"
        ""
        ".toggle-buttons button {"
        "    padding: 0.5rem;"
        "    font-size: 1rem;"
        "    background-color: #0077cc;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 5px;"
        "    cursor: pointer;"
        "    transition: background-color 0.3s;"
        "}"
        ""
        ".toggle-buttons button:hover {"
        "    background-color: #005fa3;"
        "}"
        ""
        "/* Textbox Section */"
        ".trigger-controls {"
        "    display: flex;"
        "    align-items: center; /* Vertically align label, input, and button */"
        "    gap: 0.5rem; /* Spacing between elements */"
        "    width: 100%; /* Ensure full width for alignment */"
        "    justify-content: center; /* Center the group horizontally */"
        "}"
        ""
        ".trigger-controls input {"
        "    flex-grow: 1; /* Allow the input to take up extra space */"
        "    max-width: 200px; /* Limit the width of the input */"
        "    padding: 0.5rem;"
        "    border: 1px solid #ddd;"
        "    border-radius: 5px;"
        "}"
        ""
        "button {"
        "    background-color: #0077cc;"
        "    color: white;"
        "    padding: 0.5rem;"
        "    border: none;"
        "    border-radius: 5px;"
        "    cursor: pointer;"
        "    transition: background-color 0.3s;"
        "}"
        ""
        "button:hover {"
        "    background-color: #005fa3;"
        "}"
        ""
        ".schedule-controls {"
        "    display: flex;"
        "    flex-direction: column;"
        "    align-items: center;"
        "    gap: 1rem;"
        "}"
        ""
        ".days-toggle {"
        "    display: flex;"
        "    flex-wrap: wrap;"
        "    gap: 0.5rem;"
        "    justify-content: center;"
        "}"
        ""
        ".day-button {"
        "    padding: 0.5rem 1rem;"
        "    font-size: 0.9rem;"
        "    border: 1px solid #ddd;"
        "    border-radius: 5px;"
        "    cursor: pointer;"
        "    background-color: #f5f5f5; /* Light gray background */"
        "    color: #333; /* Dark text for good contrast */"
        "    transition: background-color 0.3s, color 0.3s, border-color 0.3s;"
        "}"
        ""
        ".day-button.selected {"
        "    background-color: #0077cc;"
        "    color: white;"
        "    border-color: #0077cc;"
        "}"
        ""
        ".day-button:hover {"
        "    background-color: #d0e8ff;"
        "}";
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, css, strlen(css));
    return ESP_OK;
}

httpd_uri_t css_uri = {
    .uri = "/styles.css",
    .method = HTTP_GET,
    .handler = css_get_handler,
    .user_ctx = NULL};

esp_err_t js_get_handler(httpd_req_t *req) {
    const char *js_file =
        "let globalDays = 0;\n"
        "let globalHours = 0;\n"
        "let globalMinutes = 0;\n"
        "\n"
        "  document.querySelectorAll(\".day-button\").forEach((button) => {\n"
        "    button.addEventListener(\"click\", () => {\n"
        "      button.classList.toggle(\"selected\");\n"
        "    });\n"
        "  });\n"
        "\n"
        "  updateStats().then(() => setDayButtons()).then(() => setTimePicker()).then(() => setTriggerInput());\n"
        "\n"
        "\n"
        "  setInterval(() => {\n"
        "    updateStats();\n"
        "  }, 10000);\n"
        "\n"
        "const dayMapping = {\n"
        "  0: \"Mon\",\n"
        "  1: \"Tues\",\n"
        "  2: \"Wed\",\n"
        "  3: \"Thurs\",\n"
        "  4: \"Fri\",\n"
        "  5: \"Sat\",\n"
        "  6: \"Sun\"\n"
        "}\n"
        "\n"
        "function setDayButtons() {\n"
        "  document.querySelectorAll(\".day-button\").forEach((button) => {\n"
        "    if ((globalDays >> (parseInt(button.getAttribute('data-day'))) - 1) & 1) {\n"
        "      button.classList.toggle(\"selected\");\n"
        "    }\n"
        "  });\n"
        "}\n"
        "function setTimePicker() {\n"
        "  const timeInput = document.getElementById(\"time-input\");\n"
        "  const formattedTime = `${String(globalHours).padStart(2, '0')}:${String(globalMinutes).padStart(2, '0')}`;\n"
        "  timeInput.value = formattedTime;\n"
        "}\n"

        "function setTriggerInput() {\n"
        "const triggerInput = document.getElementById(\"trigger-input\");\n"
        "triggerInput.value = String(globalTriggerLevel);\n"
        "}\n"

        "async function updateStats() {\n"
        "  await fetch(\"/stats\")\n"
        "    .then((response) => response.json())\n"
        "    .then((data) => {\n"
        "      document.getElementById(\"water-level\").innerText = data.level;\n"
        "      document.getElementById(\"pump-state\").innerText = data.pump_state.toUpperCase() == \"TRUE\"\n"
        "        ? \"ON\"\n"
        "        : \"OFF\";\n"
        "      document.getElementById(\"current-system-time\").innerText = data.current_system_time;\n"
        "      document.getElementById(\"last-trigger-time\").innerText = data.last_trigger + ` (${data.last_reason})`;\n"
        "      globalTriggerLevel = data.trigger_level; \n"
        "      globalDays = data.topup_dates; \n"
        "      globalHours = data.topup_hour; \n"
        "      globalMinutes = data.topup_minute; \n"
        "    })\n"
        "    .catch((err) => console.error(\"Error fetching water level:\", err));\n"
        "}\n"
        "\n"
        "// Toggle the pump state\n"
        "function togglePump(state) {\n"
        "  fetch(`/pump?state=${state}`, { method: \"POST\" })\n"
        "    .then((response) => response.text())\n"
        "    .catch((err) => console.error(\"Error toggling pump:\", err));\n"
        "  updateStats();\n"
        "}\n"
        "\n"
        "// Set the trigger level\n"
        "function setTriggerLevel() {\n"
        "  const level = document.getElementById(\"trigger-input\").value;\n"
        "  fetch(`/set-trigger?level=${level}`, { method: \"POST\" })\n"
        "    .then((response) => response.text())\n"
        "    .catch((err) => console.error(\"Error setting trigger level:\", err))\n;"
        "  updateStats();\n"
        "}\n"
        "\n"
        "// Test water topup feature\n"
        "function topUp() {\n"
        "  fetch(`/topup`)\n"
        "    .then((response) => response.text())\n"
        "    .catch((err) => console.error(\"Error topping up water:\", errr));\n"
        "  updateStats();\n"
        "}\n"
        "\n"
        "function setSchedule() {"
        "  const time = document.getElementById(\"time-input\").value;\n"
        "  const selectedDays = Array.from(\n"
        "    document.querySelectorAll(\".day-button.selected\")\n"
        "  ).map((button) => button.dataset.day);\n"
        "\n"
        "  if (!time) {\n"
        "    alert(\"Please set a valid time.\");\n"
        "    return;\n"
        "  }\n"
        "  const [hours, minutes] = time.split(':').map(Number);\n"
        "\n"
        "  if (selectedDays.length === 0) {\n"
        "    alert(\"Please select at least one day.\");\n"
        "    return;\n"
        "  }\n"
        "  let days = 0;\n"
        "  selectedDays.forEach((value) => {\n"
        "    const dayValue = parseInt(value);\n"
        "    days |= 1 << (dayValue - 1);\n"
        "  });\n"
        ""
        "  let schedule = {\n"
        "    time: {\n"
        "      hours: hours,\n"
        "      minutes, minutes\n"
        "    },\n"
        "    days: days,\n"
        "  };\n"
        "\n"
        "  fetch('/topup/schedule', {\n"
        "    method: \"POST\",\n"
        "    body: JSON.stringify(schedule),\n"
        "  })\n"
        "    .catch((err) => console.error(\"Error setting new schedule\", err));\n"
        "}\n";
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_send(req, js_file, strlen(js_file));
    return ESP_OK;
}

httpd_uri_t js_uri = {
    .uri = "/script.js",
    .method = HTTP_GET,
    .handler = js_get_handler,
    .user_ctx = NULL};

esp_err_t stats_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SERVER, "Handling get statistics request");
    char response[350];
    float water_level;
    esp_err_t res = get_current_water_level(&water_level);
    if (res != ESP_OK) {
        ESP_LOGI("water fail", "error getting water level");
        water_level = -1;
    }
    float trigger_level = get_trigger_level();
    bool pump_state = get_pump_state();
    get_last_trigger();
    get_trigger_reason();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

    snprintf(response, sizeof(response), "{\"level\":%.2f,\"trigger_level\":%.2f,\"pump_state\":%s,\"current_system_time\":\"%s\", \"topup_dates\": %i, \"topup_hour\": %i, \"topup_minute\": %i, \"last_trigger\": \"%s\", \"last_reason\": \"%s\"}",
             water_level, trigger_level, pump_state ? "\"true\"" : "\"false\"", strftime_buf, get_trigger_days(), get_trigger_hour(), get_trigger_minute(), last_trigger, last_trigger_reason);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

httpd_uri_t stats_uri = {
    .uri = "/stats",
    .method = HTTP_GET,
    .handler = stats_get_handler,
    .user_ctx = NULL};

esp_err_t pump_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SERVER, "Handling set pump request");
    char *buf;
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGD(TAG_SERVER, "Found URL query => %s", buf);
            char param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN], dec_param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN] = {0};
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK) {
                ESP_LOGD(TAG_SERVER, "Found URL query parameter => state=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGD(TAG_SERVER, "Decoded query parameter => %s", dec_param);
                // TODO: handle empty parameter

                if (strcmp(dec_param, "on") == 0 || strcmp(dec_param, "ON") == 0) {
                    set_pump_state(true);
                } else {
                    set_pump_state(false);
                }
            }
        }
        free(buf);
        httpd_resp_send(req, "Pump state set", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "Must supply query parameter 'level'", HTTPD_RESP_USE_STRLEN); // TODO: use correct HTTP response code
    }
    return ESP_OK;
}

httpd_uri_t pump_uri = {
    .uri = "/pump",
    .method = HTTP_POST,
    .handler = pump_post_handler,
    .user_ctx = NULL};

esp_err_t set_trigger_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SERVER, "Handling set trigger request");
    char *buf;
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGD(TAG_SERVER, "Found URL query => %s", buf);
            char param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN], dec_param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN] = {0};
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "level", param, sizeof(param)) == ESP_OK) {
                ESP_LOGD(TAG_SERVER, "Found URL query parameter => level=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGD(TAG_SERVER, "Decoded query parameter => %s", dec_param);
                // TODO: handle empty parameter
                float level = atoff(dec_param);
                set_trigger_level(level);
            }
        }
        free(buf);
        httpd_resp_send(req, "Trigger level set", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "must supply query parameter 'level'", HTTPD_RESP_USE_STRLEN); // TODO: use correct HTTP response code
    }

    return ESP_OK;
}

httpd_uri_t set_trigger_uri = {
    .uri = "/set-trigger",
    .method = HTTP_POST,
    .handler = set_trigger_post_handler,
    .user_ctx = NULL};

esp_err_t topup_handler(httpd_req_t *req) {
    topup_task();
    httpd_resp_send(req, "Topup done", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

httpd_uri_t set_topup_uri = {
    .uri = "/topup",
    .method = HTTP_GET,
    .handler = topup_handler,
    .user_ctx = NULL};

esp_err_t topup_schedule_handler(httpd_req_t *req) {
    char content[100];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(content);
    if (!json) {
        ESP_LOGE(TAG, "Invalid JSON received");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *time = cJSON_GetObjectItem(json, "time");
    if (!time) {
        ESP_LOGE(TAG, "Missing time in JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    cJSON *json_hour = cJSON_GetObjectItem(time, "hours");
    cJSON *json_minutes = cJSON_GetObjectItem(time, "minutes");
    cJSON *json_days = cJSON_GetObjectItem(json, "days");

    if (!json_hour || !json_minutes || !json_days || !cJSON_IsNumber(json_hour) || !cJSON_IsNumber(json_minutes) || !cJSON_IsNumber(json_days)) {
        ESP_LOGE(TAG, "Missing hours/minutes or days in JSON or not numbers");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    set_trigger_days(json_days->valueint);
    set_trigger_hour(json_hour->valueint);
    set_trigger_minute(json_minutes->valueint);
    ESP_LOGI(TAG, "Schedule set: hours=%i, minutes=%i, days=%i", json_hour->valueint, json_minutes->valueint, json_days->valueint);

    cJSON_Delete(json);
    httpd_resp_sendstr(req, "Schedule set successfully");
    return ESP_OK;
}

httpd_uri_t set_topup_schedule_uri = {
    .uri = "/topup/schedule",
    .method = HTTP_POST,
    .handler = topup_schedule_handler,
    .user_ctx = NULL};

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG_SERVER, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG_SERVER, "Registering URI handlers");
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &css_uri);
        httpd_register_uri_handler(server, &js_uri);
        httpd_register_uri_handler(server, &stats_uri);
        httpd_register_uri_handler(server, &pump_uri);
        httpd_register_uri_handler(server, &set_trigger_uri);
        httpd_register_uri_handler(server, &set_topup_uri);
        httpd_register_uri_handler(server, &set_topup_schedule_uri);
        return server;
    }

    ESP_LOGI(TAG_SERVER, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server) {
    return httpd_stop(server);
}

static void disconnect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server) {
        ESP_LOGI(TAG_SERVER, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG_SERVER, "Failed to stop http server");
        }
    }
}

static void connect_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    httpd_handle_t *server = (httpd_handle_t *)arg;
    if (*server == NULL) {
        ESP_LOGI(TAG_SERVER, "Starting webserver");
        *server = start_webserver();
    }
}

static void obtain_time(void) {
    ESP_LOGI(TAG_TIME, "Initializing and starting SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 15;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count) {
        ESP_LOGI(TAG_TIME, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    }
    time(&now);
    localtime_r(&now, &timeinfo);

    esp_netif_sntp_deinit();
}

void topup_task() {
    doTopup = false;
    ESP_LOGI(TAG, "Performing topup");
    float water_level;
    esp_err_t err = get_current_water_level(&water_level);
    int num_below = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAILED TO GET WATER LEVEL - NOT TOPPING UP WATER");
        return;
    }
    float trigger_level = get_trigger_level();
    if (water_level >= trigger_level) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        char strftime_buf[64];
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        set_last_trigger(strftime_buf);

        bool earlyBreak = false;
        pump_on();
        volatile int64_t start_time = esp_timer_get_time();
        // Update water level BEFORE entering the loop
        while (num_below < NUM_BELOW_TRIGGER) {
            err = get_current_water_level(&water_level);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Sensor not ok, abandoning topup");
                set_trigger_reason(SENSOR_ERROR);
                earlyBreak = true;
                break;
            }

            if (water_level < trigger_level) {
                num_below++;
            } else {
                num_below = 0;
            }
            if (timeout_expired(start_time, MAX_TOPUP_TIME)) {
                set_trigger_reason(PUMP_TIMEOUT);
                ESP_LOGW(TAG_TIME, "10s timer topup reached... stopping pump");
                earlyBreak = true;
                break;
            }
        }
        pump_off();
        if (!earlyBreak) {
            set_trigger_reason(TRIGGER_REACHED);
        }
    } else {
        set_trigger_reason(TOPUP_NOT_NEEDED);
    }
    ESP_LOGI(TAG, "Topup done");
}

void timer_callback(void *arg) {
    static int prev_day_executed = -1; // possible for timer to occur multiple times during the trigger period, this ensures it only executes once per day
    time_t now;
    struct tm timeinfo;
    time(&now);

    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG_TIME, "The timer triggered at: %s", strftime_buf);

    // tm_wday stores Sunday as 0, when I need Monday as 0. Subtracting 1 and manually setting Sunday to 6 fixes this.
    int currentWeekday = timeinfo.tm_wday - 1;
    currentWeekday = currentWeekday < 0 ? 6 : currentWeekday;

    if (((get_trigger_days() >> currentWeekday) & 1) && timeinfo.tm_hour == get_trigger_hour() && timeinfo.tm_min == get_trigger_minute() && prev_day_executed != timeinfo.tm_mday) {
        ESP_LOGI(TAG_TIME, "Timer has triggered topup function");
        obtain_time(); // resync with SNTP
        prev_day_executed = timeinfo.tm_mday;
        doTopup = true;
    }
}

void start_timer() {
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
        .name = "MinuteTimer"};

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);

    int64_t delay_us = 60 * 1000000;
    esp_timer_start_periodic(timer, delay_us);
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("example_common", ESP_LOG_INFO); // To print the IP address

    // Set up HC-SR04 sensor
    distance_init(&sensor);
    ESP_ERROR_CHECK(gpio_reset_pin(PUMP_PIN));
    ESP_ERROR_CHECK(gpio_set_direction(PUMP_PIN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(PUMP_PIN, 0));

    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_handle));

    ESP_ERROR_CHECK(example_connect());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    server = start_webserver();

    ++boot_count;
    ESP_LOGI(TAG_TIME, "Boot count: %d", boot_count);

    // Set system time using SNTP
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG_TIME, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        time(&now);
    }
    char strftime_buf[64];
    setenv("TZ", "SAST-2", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG_TIME, "The current date/time in Johannesburg is: %s", strftime_buf);

    start_timer();
    while (server) {
        if (doTopup) {
            topup_task();
        }
        sleep(5);
    }
}
