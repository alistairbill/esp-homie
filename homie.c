#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "homie.h"

static const char *TAG = "HOMIE";
static esp_mqtt_client_handle_t client;
static homie_config_t *config;
volatile EventGroupHandle_t homie_event_group;

int homie_remove_retained(const char *topic)
{
    // TODO: remove this once we can access 'retained' in message receive callback
    return homie_publish(topic, QOS_1, RETAINED, "");
}

static void homie_handle_command(const char *topic, const char *data)
{
    if (*data == '\0') {
        ESP_LOGW(TAG, "received empty command");
        return;
    }
    char base_topic[HOMIE_MAX_TOPIC_LEN];
    homie_mktopic(base_topic, "");
    int base_len = strlen(base_topic);
    if (strncmp(base_topic, topic, base_len) != 0) {
        ESP_LOGE(TAG, "received non-homie topic");
        return;
    }

    const char *subtopic = topic + base_len;
    if (config->msg_handler) {
        config->msg_handler(subtopic, data);
    }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    static char *topic;
    static char *data_text;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(homie_event_group,
            HOMIE_MQTT_CONNECTED_BIT | HOMIE_MQTT_UPDATE_REQUIRED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(homie_event_group, HOMIE_MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        if (event->current_data_offset == 0) {
            topic = malloc(event->topic_len + 1);
            if (topic == NULL) {
                goto free;
            }
            memset(topic, 0, 1);
            strlcpy(topic, event->topic, event->topic_len + 1);
            data_text = malloc(event->total_data_len + 1);
            if (data_text == NULL) {
                goto free;
            }
            memset(data_text, 0, 1);
        }
        if (topic == NULL || data_text == NULL) {
            goto free;
        }
        if (event->total_data_len > 0) {
            strlcat(data_text, event->data, event->data_len + 1);
        }
        if (event->current_data_offset + event->data_len >= event->total_data_len) {
            homie_handle_command(topic, data_text);
        }
    free:
        free(topic);
        topic = NULL;
        free(data_text);
        data_text = NULL;
        break;

    case MQTT_EVENT_BEFORE_CONNECT:
    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
    case MQTT_EVENT_ERROR:
    case MQTT_EVENT_ANY:
        break;
    }

    return ESP_OK;
}

static void mqtt_app_start(void)
{
    char lwt_topic[HOMIE_MAX_TOPIC_LEN];
    homie_mktopic(lwt_topic, "$state");

    config->mqtt_config.event_handle = mqtt_event_handler;
    config->mqtt_config.lwt_topic = lwt_topic;
    config->mqtt_config.lwt_msg = "lost";
    config->mqtt_config.lwt_qos = 1;
    config->mqtt_config.lwt_retain = 1;

    client = esp_mqtt_client_init(&config->mqtt_config);
    esp_mqtt_client_start(client);
}

void homie_mktopic(char *topic, const char *subtopic)
{
    snprintf(topic, HOMIE_MAX_TOPIC_LEN, "%s/%s", config->base_topic, subtopic);
}

void homie_subscribe(const char *subtopic, const int qos)
{
    int msg_id;
    char topic[HOMIE_MAX_TOPIC_LEN];
    homie_mktopic(topic, subtopic);

    msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
}

int homie_publish(const char *subtopic, int qos, int retain, const char *payload)
{
    if (config == NULL) {
        ESP_LOGW(TAG, "Attempted to publish before homie connected");
        return -2;
    }
    ESP_LOGD(TAG, "publishing %s: %s", subtopic, payload);

    char topic[HOMIE_MAX_TOPIC_LEN];
    homie_mktopic(topic, subtopic);

    return esp_mqtt_client_publish(client, topic, payload, 0, qos, retain);
}

int homie_publishf(const char *subtopic, int qos, int retain, const char *format, ...)
{
    char payload_string[64];
    va_list argptr;
    va_start(argptr, format);
    vsnprintf(payload_string, 64, format, argptr);
    va_end(argptr);
    return homie_publish(subtopic, qos, retain, payload_string);
}

int homie_publish_int(const char *subtopic, int qos, int retain, int payload)
{
    char payload_string[16];
    snprintf(payload_string, 16, "%d", payload);
    return homie_publish(subtopic, qos, retain, payload_string);
}

static inline int clamp(int n, int lower, int upper)
{
    return n <= lower ? lower : (n >= upper ? upper : n);
}

static int8_t get_wifi_rssi(void)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get wifi info");
        return 0;
    }
    return info.rssi;
}

static void get_ip(char *ip_string)
{
    tcpip_adapter_ip_info_t ip;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
    sprintf(ip_string, "%u.%u.%u.%u", (ip.ip.addr & 0x000000ff), (ip.ip.addr & 0x0000ff00) >> 8,
            (ip.ip.addr & 0x00ff0000) >> 16, (ip.ip.addr & 0xff000000) >> 24);
}

static void get_mac(char *mac_string)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    sprintf(mac_string, "%X:%X:%X:%X:%X:%X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void publish_attributes(void)
{
    char mac_address[] = "00:00:00:00:00:00";
    char ip_address[16];
    get_ip(ip_address);
    get_mac(mac_address);
    const esp_app_desc_t *app = esp_ota_get_app_description();
    homie_publish("$homie", QOS_1, RETAINED, "4.0.1");
    homie_publish("$name", QOS_1, RETAINED, config->device_name);
    homie_publish("$localip", QOS_1, RETAINED, ip_address);
    homie_publish("$mac", QOS_1, RETAINED, mac_address);
    homie_publish("$nodes", QOS_1, RETAINED, config->node_list);
    homie_publish("$extensions", QOS_1, RETAINED,
        "org.homie.legacy-stats:0.1.1:[4.x],org.homie.legacy-firmware:0.1.1:[4.x]");
    homie_publish("$implementation", QOS_1, RETAINED, "ESP8266_RTOS_SDK");
    homie_publish("$stats", QOS_1, RETAINED, "interval,uptime,signal,freeheap");
    homie_publish_int("$stats/interval", QOS_1, RETAINED, config->stats_interval);
    homie_publish("$fw/name", QOS_1, RETAINED, app->project_name);
    homie_publish("$fw/version", QOS_1, RETAINED, app->version);
}

static void publish_stats(void)
{
    int rssi = get_wifi_rssi();
    homie_publish_int("$stats/signal", QOS_1, RETAINED, clamp((rssi + 100) * 2, 0, 100));
    homie_publish_int("$stats/freeheap", QOS_1, RETAINED, esp_get_free_heap_size());
    homie_publish_int("$stats/uptime", QOS_1, RETAINED, esp_timer_get_time() / 1000000);
}

static void homie_connected(void)
{
    homie_publish("$state", QOS_1, RETAINED, "init");
    if (!config->disable_publish_attributes) {
        publish_attributes();
    }
    publish_stats();
    homie_publish("$state", QOS_1, RETAINED, "ready");
    if (config->connected_handler) {
        config->connected_handler();
    }
    xEventGroupClearBits(homie_event_group, HOMIE_MQTT_UPDATE_REQUIRED_BIT);
}

static void homie_task(void *args)
{
    EventBits_t bits = HOMIE_MQTT_CONNECTED_BIT;
    while ((xEventGroupWaitBits(homie_event_group, bits, pdFALSE, pdTRUE, portMAX_DELAY) & bits) != bits)
    {} // wait for connection

    homie_connected();
    while (config->loop) {
        bits = xEventGroupWaitBits(homie_event_group, HOMIE_MQTT_UPDATE_REQUIRED_BIT,
            pdFALSE, pdTRUE, config->stats_interval * 1000 / portTICK_PERIOD_MS);
        if (bits & HOMIE_MQTT_UPDATE_REQUIRED_BIT) {
            homie_connected();
        } else {
            publish_stats();
        }
    }
    vTaskDelete(NULL);
}

void homie_init(homie_config_t *passed_config)
{
    config = passed_config;
    homie_event_group = xEventGroupCreate();
    mqtt_app_start();
    xTaskCreate(&homie_task, "homie_task", configMINIMAL_STACK_SIZE * 10, NULL, 5, NULL);
}

void homie_disconnect(void)
{
    esp_mqtt_client_stop(client);
}
