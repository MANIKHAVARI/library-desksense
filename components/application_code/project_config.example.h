#pragma once

/*
 * Copy this file to project_config.h and replace the example values.
 * project_config.h is ignored by Git so credentials remain local.
 */

#define WIFI_NAME              "YOUR_2_4_GHZ_WIFI_NAME"
#define WIFI_PASSWORD          "YOUR_WIFI_PASSWORD"

/*
 * Use the LAN IPv4 address of the computer running the backend services.
 * The ESP32 and computer must be connected to the same network.
 */
#define BACKEND_HOST           "192.168.1.100"

#define MQTT_BROKER            "mqtt://" BACKEND_HOST ":1883"
#define HTTP_TELEMETRY_URL     "http://" BACKEND_HOST ":18080/telemetry"
#define HTTP_HOST_HEADER       BACKEND_HOST ":18080"
#define COAP_TELEMETRY_URI     "coap://" BACKEND_HOST ":5683/telemetry"
