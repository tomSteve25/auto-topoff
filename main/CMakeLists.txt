idf_build_get_property(target IDF_TARGET)

# Define the required components
set(requires 
    esp_wifi
    esp_event
    esp_netif
    esp_http_server
    nvs_flash
    protocol_examples_common
    json  # For cJSON library
)

# Add conditional components for Linux
if(${target} STREQUAL "linux")
    list(APPEND requires esp_stubs esp-tls)
endif()

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${requires})
