// This file reads the sensors and communicates using Wi-Fi, MQTT, HTTP, and CoAP.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "coap_config.h"
#include "coap3/coap.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "project_config.h"

#define MQTT_STATUS_TOPIC      "library/desk1/status"
#define MQTT_EVENT_TOPIC       "library/desk1/event"
#define MQTT_CONFIG_TOPIC      "library/desk1/config/#"
#define MQTT_CONFIG_ACK_TOPIC  "library/desk1/config/ack"
#define MQTT_METRICS_TOPIC     "library/desk1/metrics"

#define PIR_PIN                GPIO_NUM_18
#define SOUND_PIN              GPIO_NUM_32
#define LDR_CHANNEL            ADC_CHANNEL_6

typedef struct
{
    int occupied;
    int occupancy_duration;
    int last_session_duration;
    int light;
    int poor_light;
    int sound_edges;
    int high_noise;
} sensor_data_t;

static sensor_data_t data;

static SemaphoreHandle_t data_mutex;
static adc_oneshot_unit_handle_t adc_handle;
static esp_mqtt_client_handle_t mqtt_client = NULL;

static volatile uint32_t sound_counter = 0;

static volatile int wifi_connected = 0;
static volatile int mqtt_connected = 0;

static volatile int occupancy_timeout_setting = 30;
static volatile int light_threshold_setting = 1800;
static volatile int noise_threshold_setting = 10;
static volatile int sampling_rate_setting = 1;
static volatile int communication_mode_setting = 0;

static volatile int coap_response_received = 0;
static volatile int coap_response_success = 0;

static portMUX_TYPE sound_lock =
    portMUX_INITIALIZER_UNLOCKED;


// This interrupt counts every falling edge from the digital sound sensor.
static void IRAM_ATTR sound_isr(void *argument)
{
    portENTER_CRITICAL_ISR(&sound_lock);

    sound_counter++;

    portEXIT_CRITICAL_ISR(&sound_lock);
}


// This function publishes one environmental event through MQTT.
static void publish_event(const char *message)
{
    if (!mqtt_connected)
        return;

    esp_mqtt_client_publish(
        mqtt_client,
        MQTT_EVENT_TOPIC,
        message,
        0,
        1,
        0
    );

    printf(
        "MQTT event: %s\n",
        message
    );
}


// This function publishes confirmation after changing a setting.
static void publish_config_ack(const char *message)
{
    printf(
        "MQTT config: %s\n",
        message
    );

    if (!mqtt_connected)
        return;

    esp_mqtt_client_publish(
        mqtt_client,
        MQTT_CONFIG_ACK_TOPIC,
        message,
        0,
        1,
        0
    );
}


// This function estimates HTTP request overhead including IPv4 and TCP headers.
static int estimate_http_overhead_bytes(
    int payload_bytes
)
{
    char request_headers[256];

    int header_bytes =
        snprintf(
            request_headers,
            sizeof(request_headers),
            "POST /telemetry HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n",
            HTTP_HOST_HEADER,
            payload_bytes
        );

    if (header_bytes < 0)
    {
        header_bytes = 0;
    }

    return header_bytes + 40;
}


// This function estimates CoAP request overhead including IPv4 and UDP headers.
static int estimate_coap_overhead_bytes(
    size_t token_length
)
{
    int coap_base_header = 4;
    int uri_path_option = 10;
    int content_format_option = 2;
    int payload_marker = 1;
    int ipv4_udp_headers = 28;

    return coap_base_header +
           (int)token_length +
           uri_path_option +
           content_format_option +
           payload_marker +
           ipv4_udp_headers;
}


// This function publishes one communication measurement through MQTT.
static void publish_communication_metric(
    const char *protocol,
    int64_t latency_ms,
    int payload_bytes,
    int overhead_bytes,
    int success
)
{
    char metric_message[220];

    int total_bytes =
        payload_bytes +
        overhead_bytes;

    snprintf(
        metric_message,
        sizeof(metric_message),
        "{"
        "\"protocol\":\"%s\","
        "\"latency_ms\":%lld,"
        "\"payload_bytes\":%d,"
        "\"overhead_bytes\":%d,"
        "\"total_bytes\":%d,"
        "\"success\":%d"
        "}",
        protocol,
        (long long)latency_ms,
        payload_bytes,
        overhead_bytes,
        total_bytes,
        success
    );

    printf(
        "%s metric | Latency: %lld ms | "
        "Payload: %d B | Overhead: %d B | "
        "Total: %d B | Success: %d\n",
        protocol,
        (long long)latency_ms,
        payload_bytes,
        overhead_bytes,
        total_bytes,
        success
    );

    if (!mqtt_connected)
    {
        return;
    }

    esp_mqtt_client_publish(
        mqtt_client,
        MQTT_METRICS_TOPIC,
        metric_message,
        0,
        1,
        0
    );
}


// This function processes configuration messages received through MQTT.
static void handle_config_message(
    const char *topic,
    const char *message
)
{
    int value;
    char reply[80];

    if (strcmp(
            topic,
            "library/desk1/config/noise_threshold"
        ) == 0)
    {
        if (sscanf(message, "%d", &value) == 1 &&
            value >= 1 &&
            value <= 1000)
        {
            noise_threshold_setting = value;

            snprintf(
                reply,
                sizeof(reply),
                "noise_threshold=%d",
                value
            );

            publish_config_ack(reply);
        }
        else
        {
            publish_config_ack(
                "invalid noise_threshold"
            );
        }
    }

    else if (strcmp(
                 topic,
                 "library/desk1/config/light_threshold"
             ) == 0)
    {
        if (sscanf(message, "%d", &value) == 1 &&
            value >= 0 &&
            value <= 4095)
        {
            light_threshold_setting = value;

            snprintf(
                reply,
                sizeof(reply),
                "light_threshold=%d",
                value
            );

            publish_config_ack(reply);
        }
        else
        {
            publish_config_ack(
                "invalid light_threshold"
            );
        }
    }

    else if (strcmp(
                 topic,
                 "library/desk1/config/occupancy_timeout"
             ) == 0)
    {
        if (sscanf(message, "%d", &value) == 1 &&
            value >= 1 &&
            value <= 3600)
        {
            occupancy_timeout_setting = value;

            snprintf(
                reply,
                sizeof(reply),
                "occupancy_timeout=%d",
                value
            );

            publish_config_ack(reply);
        }
        else
        {
            publish_config_ack(
                "invalid occupancy_timeout"
            );
        }
    }

    else if (strcmp(
                 topic,
                 "library/desk1/config/sampling_rate"
             ) == 0)
    {
        if (sscanf(message, "%d", &value) == 1 &&
            value >= 1 &&
            value <= 60)
        {
            sampling_rate_setting = value;

            snprintf(
                reply,
                sizeof(reply),
                "sampling_rate=%d",
                value
            );

            publish_config_ack(reply);
        }
        else
        {
            publish_config_ack(
                "invalid sampling_rate"
            );
        }
    }

    else if (strcmp(
                 topic,
                 "library/desk1/config/communication_mode"
             ) == 0)
    {
        if (strcmp(message, "http") == 0)
        {
            communication_mode_setting = 0;

            publish_config_ack(
                "communication_mode=http"
            );
        }

        else if (strcmp(message, "coap") == 0)
        {
            communication_mode_setting = 1;

            publish_config_ack(
                "communication_mode=coap"
            );
        }

        else
        {
            publish_config_ack(
                "invalid communication_mode"
            );
        }
    }
}


// This function receives connection, disconnection, and data events from MQTT.
static void mqtt_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_id == MQTT_EVENT_CONNECTED)
    {
        mqtt_connected = 1;

        printf("MQTT connected.\n");

        esp_mqtt_client_publish(
            mqtt_client,
            MQTT_STATUS_TOPIC,
            "online",
            0,
            1,
            1
        );

        esp_mqtt_client_subscribe(
            mqtt_client,
            MQTT_CONFIG_TOPIC,
            1
        );

        printf(
            "Subscribed to MQTT configuration topics.\n"
        );
    }

    else if (event_id == MQTT_EVENT_DISCONNECTED)
    {
        mqtt_connected = 0;

        printf("MQTT disconnected.\n");
    }

    else if (event_id == MQTT_EVENT_DATA)
    {
        esp_mqtt_event_handle_t event =
            (esp_mqtt_event_handle_t)event_data;

        char topic[100];
        char message[40];

        int topic_length =
            event->topic_len;

        int message_length =
            event->data_len;

        if (topic_length >= (int)sizeof(topic))
        {
            topic_length =
                sizeof(topic) - 1;
        }

        if (message_length >= (int)sizeof(message))
        {
            message_length =
                sizeof(message) - 1;
        }

        memcpy(
            topic,
            event->topic,
            topic_length
        );

        topic[topic_length] = '\0';

        memcpy(
            message,
            event->data,
            message_length
        );

        message[message_length] = '\0';

        handle_config_message(
            topic,
            message
        );
    }

    else if (event_id == MQTT_EVENT_ERROR)
    {
        printf(
            "MQTT connection error.\n"
        );
    }
}


// This function starts the MQTT client and connects it to Mosquitto.
static void start_mqtt(void)
{
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = MQTT_BROKER
    };

    mqtt_client =
        esp_mqtt_client_init(&mqtt_config);

    configASSERT(
        mqtt_client != NULL
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(
            mqtt_client
        )
    );
}


// This function handles Wi-Fi connection and reconnection events.
static void wifi_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_connected = 0;

        printf(
            "Wi-Fi disconnected. Reconnecting...\n"
        );

        esp_wifi_connect();
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        wifi_connected = 1;

        printf(
            "Wi-Fi connected. IP address: " IPSTR "\n",
            IP2STR(&event->ip_info.ip)
        );

        if (mqtt_client == NULL)
        {
            start_mqtt();
        }
    }
}


// This function initializes Wi-Fi in station mode.
static void start_wifi(void)
{
    esp_err_t result =
        nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );
    }
    else
    {
        ESP_ERROR_CHECK(result);
    }

    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&wifi_init)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_NAME,
            .password = WIFI_PASSWORD
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );
}


// This function sends one JSON telemetry report through HTTP.
static void send_http_telemetry(
    sensor_data_t current
)
{
    char json[256];

    snprintf(
        json,
        sizeof(json),
        "{"
        "\"occupied\":%d,"
        "\"occupancy_duration\":%d,"
        "\"light\":%d,"
        "\"poor_light\":%d,"
        "\"sound_edges\":%d,"
        "\"high_noise\":%d"
        "}",
        current.occupied,
        current.occupancy_duration,
        current.light,
        current.poor_light,
        current.sound_edges,
        current.high_noise
    );

    int payload_bytes =
        strlen(json);

    int overhead_bytes =
        estimate_http_overhead_bytes(
            payload_bytes
        );

    int64_t start_time_us =
        esp_timer_get_time();

    int success = 0;

    esp_http_client_config_t http_config = {
        .url = HTTP_TELEMETRY_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000
    };

    esp_http_client_handle_t http_client =
        esp_http_client_init(&http_config);

    if (http_client == NULL)
    {
        int64_t latency_ms =
            (esp_timer_get_time() -
             start_time_us +
             999) /
            1000;

        printf(
            "HTTP client could not start.\n"
        );

        publish_communication_metric(
            "http",
            latency_ms,
            payload_bytes,
            overhead_bytes,
            0
        );

        return;
    }

    esp_http_client_set_header(
        http_client,
        "Content-Type",
        "application/json"
    );

    esp_http_client_set_post_field(
        http_client,
        json,
        payload_bytes
    );

    esp_err_t result =
        esp_http_client_perform(
            http_client
        );

    if (result == ESP_OK)
    {
        int status =
            esp_http_client_get_status_code(
                http_client
            );

        success =
            status >= 200 &&
            status < 300;

        printf(
            "HTTP telemetry sent. Status: %d\n",
            status
        );
    }
    else
    {
        printf(
            "HTTP telemetry failed: %s\n",
            esp_err_to_name(result)
        );
    }

    int64_t latency_ms =
        (esp_timer_get_time() -
         start_time_us +
         999) /
        1000;

    esp_http_client_cleanup(
        http_client
    );

    publish_communication_metric(
        "http",
        latency_ms,
        payload_bytes,
        overhead_bytes,
        success
    );
}


// This function receives the response from the CoAP server.
static coap_response_t coap_response_handler(
    coap_session_t *session,
    const coap_pdu_t *sent,
    const coap_pdu_t *received,
    const coap_mid_t mid
)
{
    coap_pdu_code_t response_code =
        coap_pdu_get_code(received);

    coap_response_received = 1;

    if (COAP_RESPONSE_CLASS(response_code) == 2)
    {
        coap_response_success = 1;
    }
    else
    {
        coap_response_success = 0;
    }

    printf(
        "CoAP response: %d.%02d\n",
        response_code >> 5,
        response_code & 0x1F
    );

    return COAP_RESPONSE_OK;
}


// This function sends one JSON telemetry report through CoAP.
static void send_coap_telemetry(
    sensor_data_t current
)
{
    char json[256];

    coap_context_t *context = NULL;
    coap_session_t *session = NULL;
    coap_pdu_t *request = NULL;
    coap_optlist_t *options = NULL;
    coap_addr_info_t *address_information = NULL;

    coap_uri_t uri;
    coap_address_t destination_address;
    coap_proto_t protocol;

    unsigned char uri_buffer[80];
    unsigned char option_buffer[4];
    unsigned char token[8];

    size_t token_length =
        sizeof(token);

    snprintf(
        json,
        sizeof(json),
        "{"
        "\"occupied\":%d,"
        "\"occupancy_duration\":%d,"
        "\"light\":%d,"
        "\"poor_light\":%d,"
        "\"sound_edges\":%d,"
        "\"high_noise\":%d"
        "}",
        current.occupied,
        current.occupancy_duration,
        current.light,
        current.poor_light,
        current.sound_edges,
        current.high_noise
    );

    int payload_bytes =
        strlen(json);

    int overhead_bytes =
        estimate_coap_overhead_bytes(
            token_length
        );

    int64_t start_time_us =
        esp_timer_get_time();

    int success = 0;

    coap_startup();

    context =
        coap_new_context(NULL);

    if (context == NULL)
    {
        printf(
            "CoAP context could not start.\n"
        );

        goto cleanup;
    }

    coap_context_set_block_mode(
        context,
        COAP_BLOCK_USE_LIBCOAP |
        COAP_BLOCK_SINGLE_BODY
    );

    coap_register_response_handler(
        context,
        coap_response_handler
    );

    if (coap_split_uri(
            (const uint8_t *)COAP_TELEMETRY_URI,
            strlen(COAP_TELEMETRY_URI),
            &uri
        ) == -1)
    {
        printf(
            "CoAP URI is invalid.\n"
        );

        goto cleanup;
    }

    address_information =
        coap_resolve_address_info(
            &uri.host,
            uri.port,
            uri.port,
            uri.port,
            uri.port,
            0,
            1 << uri.scheme,
            COAP_RESOLVE_TYPE_REMOTE
        );

    if (address_information == NULL)
    {
        printf(
            "CoAP address could not be resolved.\n"
        );

        goto cleanup;
    }

    protocol =
        address_information->proto;

    memcpy(
        &destination_address,
        &address_information->addr,
        sizeof(destination_address)
    );

    coap_free_address_info(
        address_information
    );

    address_information = NULL;

    if (coap_uri_into_options(
            &uri,
            &destination_address,
            &options,
            1,
            uri_buffer,
            sizeof(uri_buffer)
        ) < 0)
    {
        printf(
            "CoAP URI options could not be created.\n"
        );

        goto cleanup;
    }

    session =
        coap_new_client_session(
            context,
            NULL,
            &destination_address,
            protocol
        );

    if (session == NULL)
    {
        printf(
            "CoAP session could not start.\n"
        );

        goto cleanup;
    }

    request =
        coap_new_pdu(
            COAP_MESSAGE_CON,
            COAP_REQUEST_CODE_POST,
            session
        );

    if (request == NULL)
    {
        printf(
            "CoAP request could not be created.\n"
        );

        goto cleanup;
    }

    coap_session_new_token(
        session,
        &token_length,
        token
    );

    overhead_bytes =
        estimate_coap_overhead_bytes(
            token_length
        );

    coap_add_token(
        request,
        token_length,
        token
    );

    size_t option_length =
        coap_encode_var_safe(
            option_buffer,
            sizeof(option_buffer),
            COAP_MEDIATYPE_APPLICATION_JSON
        );

    coap_insert_optlist(
        &options,
        coap_new_optlist(
            COAP_OPTION_CONTENT_FORMAT,
            option_length,
            option_buffer
        )
    );

    coap_add_optlist_pdu(
        request,
        &options
    );

    coap_add_data_large_request(
        session,
        request,
        payload_bytes,
        (const uint8_t *)json,
        NULL,
        NULL
    );

    coap_response_received = 0;
    coap_response_success = 0;

    coap_mid_t message_id =
        coap_send(
            session,
            request
        );

    if (message_id == COAP_INVALID_MID)
    {
        printf(
            "CoAP telemetry could not be sent.\n"
        );

        goto cleanup;
    }

    TickType_t response_start_time =
        xTaskGetTickCount();

    while (!coap_response_received)
    {
        int result =
            coap_io_process(
                context,
                200
            );

        if (result < 0)
        {
            printf(
                "CoAP network processing failed.\n"
            );

            break;
        }

        TickType_t elapsed_time =
            xTaskGetTickCount() -
            response_start_time;

        if (elapsed_time >=
            pdMS_TO_TICKS(3000))
        {
            break;
        }
    }

    if (coap_response_received &&
        coap_response_success)
    {
        success = 1;

        printf(
            "CoAP telemetry sent successfully.\n"
        );
    }
    else
    {
        printf(
            "CoAP telemetry failed or timed out.\n"
        );
    }

cleanup:

    if (address_information != NULL)
    {
        coap_free_address_info(
            address_information
        );
    }

    if (options != NULL)
    {
        coap_delete_optlist(
            options
        );

        options = NULL;
    }

    if (session != NULL)
    {
        coap_session_release(
            session
        );
    }

    if (context != NULL)
    {
        coap_free_context(
            context
        );
    }

    coap_cleanup();

    int64_t latency_ms =
        (esp_timer_get_time() -
         start_time_us +
         999) /
        1000;

    publish_communication_metric(
        "coap",
        latency_ms,
        payload_bytes,
        overhead_bytes,
        success
    );
}


// This task checks the PIR sensor and manages the occupancy timeout.
static void occupancy_task(void *argument)
{
    int time_left = 0;
    int previous_occupied = 0;
    int session_duration = 0;

    while (1)
    {
        if (gpio_get_level(PIR_PIN))
        {
            time_left =
                occupancy_timeout_setting;
        }

        else if (time_left > 0)
        {
            time_left--;
        }

        if (time_left >
            occupancy_timeout_setting)
        {
            time_left =
                occupancy_timeout_setting;
        }

        int occupied_now =
            time_left > 0;

        if (occupied_now)
        {
            if (!previous_occupied)
            {
                session_duration = 0;
            }

            session_duration++;
        }

        xSemaphoreTake(
            data_mutex,
            portMAX_DELAY
        );

        data.occupied =
            occupied_now;

        if (occupied_now)
        {
            data.occupancy_duration =
                session_duration;
        }
        else
        {
            data.occupancy_duration = 0;

            if (previous_occupied)
            {
                data.last_session_duration =
                    session_duration;
            }
        }

        xSemaphoreGive(
            data_mutex
        );

        if (!occupied_now)
        {
            session_duration = 0;
        }

        previous_occupied =
            occupied_now;

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}


// This task reads the LDR and determines whether the lighting is poor.
static void light_task(void *argument)
{
    int light_value;

    while (1)
    {
        ESP_ERROR_CHECK(
            adc_oneshot_read(
                adc_handle,
                LDR_CHANNEL,
                &light_value
            )
        );

        xSemaphoreTake(
            data_mutex,
            portMAX_DELAY
        );

        data.light =
            light_value;

        data.poor_light =
            light_value <
            light_threshold_setting;

        xSemaphoreGive(
            data_mutex
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}


// This task stores the number of sound edges detected during each second.
static void sound_task(void *argument)
{
    int edges;

    while (1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );

        portENTER_CRITICAL(
            &sound_lock
        );

        edges =
            sound_counter;

        sound_counter = 0;

        portEXIT_CRITICAL(
            &sound_lock
        );

        xSemaphoreTake(
            data_mutex,
            portMAX_DELAY
        );

        data.sound_edges =
            edges;

        data.high_noise =
            edges >=
            noise_threshold_setting;

        xSemaphoreGive(
            data_mutex
        );
    }
}


// This task sends periodic telemetry using the selected communication mode.
static void telemetry_task(void *argument)
{
    sensor_data_t current;

    while (1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(
                sampling_rate_setting * 1000
            )
        );

        xSemaphoreTake(
            data_mutex,
            portMAX_DELAY
        );

        current = data;

        xSemaphoreGive(
            data_mutex
        );

        const char *mode =
            communication_mode_setting == 0
                ? "HTTP"
                : "CoAP";

        printf(
            "Wi-Fi: %d | MQTT: %d | Occupancy: %d | "
            "Duration: %ds | Light: %d | Poor light: %d | "
            "Sound edges: %d | High noise: %d | "
            "Rate: %ds | Mode: %s\n",
            wifi_connected,
            mqtt_connected,
            current.occupied,
            current.occupancy_duration,
            current.light,
            current.poor_light,
            current.sound_edges,
            current.high_noise,
            sampling_rate_setting,
            mode
        );

        if (!wifi_connected)
        {
            continue;
        }

        if (communication_mode_setting == 0)
        {
            send_http_telemetry(
                current
            );
        }
        else
        {
            send_coap_telemetry(
                current
            );
        }
    }
}


// This task publishes MQTT events whenever sensor conditions change.
static void event_task(void *argument)
{
    sensor_data_t current;

    int previous_occupied = 0;
    int previous_high_noise = 0;
    int previous_poor_light = 0;

    while (1)
    {
        vTaskDelay(
            pdMS_TO_TICKS(500)
        );

        xSemaphoreTake(
            data_mutex,
            portMAX_DELAY
        );

        current = data;

        xSemaphoreGive(
            data_mutex
        );

        if (!mqtt_connected)
            continue;

        if (current.occupied !=
            previous_occupied)
        {
            if (current.occupied)
            {
                publish_event(
                    "desk_occupied"
                );
            }
            else
            {
                char release_message[96];

                snprintf(
                    release_message,
                    sizeof(release_message),
                    "{\"event\":\"desk_released\","
                    "\"duration_seconds\":%d}",
                    current.last_session_duration
                );

                publish_event(
                    release_message
                );
            }
        }

        if (current.high_noise &&
            !previous_high_noise)
        {
            publish_event(
                "high_noise"
            );
        }

        if (current.poor_light &&
            !previous_poor_light)
        {
            publish_event(
                "poor_lighting"
            );
        }

        previous_occupied =
            current.occupied;

        previous_high_noise =
            current.high_noise;

        previous_poor_light =
            current.poor_light;
    }
}


// This function initializes the sensors and starts all project tasks.
void app_main(void)
{
    ESP_ERROR_CHECK(
        gpio_set_direction(
            PIR_PIN,
            GPIO_MODE_INPUT
        )
    );

    ESP_ERROR_CHECK(
        gpio_set_direction(
            SOUND_PIN,
            GPIO_MODE_INPUT
        )
    );

    ESP_ERROR_CHECK(
        gpio_set_intr_type(
            SOUND_PIN,
            GPIO_INTR_NEGEDGE
        )
    );

    ESP_ERROR_CHECK(
        gpio_install_isr_service(0)
    );

    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            SOUND_PIN,
            sound_isr,
            NULL
        )
    );

    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = ADC_UNIT_1
    };

    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &adc_config,
            &adc_handle
        )
    );

    adc_oneshot_chan_cfg_t ldr_config = {
        .bitwidth =
            ADC_BITWIDTH_DEFAULT,

        .atten =
            ADC_ATTEN_DB_12
    };

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc_handle,
            LDR_CHANNEL,
            &ldr_config
        )
    );

    data_mutex =
        xSemaphoreCreateMutex();

    configASSERT(
        data_mutex != NULL
    );

    xTaskCreate(
        occupancy_task,
        "occupancy",
        2048,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        light_task,
        "light",
        2048,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        sound_task,
        "sound",
        2048,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        telemetry_task,
        "telemetry",
        8192,
        NULL,
        4,
        NULL
    );

    xTaskCreate(
        event_task,
        "mqtt_events",
        3072,
        NULL,
        4,
        NULL
    );

    start_wifi();
}
