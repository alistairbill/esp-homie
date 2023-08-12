#ifndef __HOMIE_H__
#define __HOMIE_H__

#include <mqtt_client.h>

#define HOMIE_MAX_TOPIC_LEN (64)
#define HOMIE_MAX_DEVICE_NAME_LEN (32)
#define HOMIE_MAX_BASE_TOPIC_LEN (32)
#define HOMIE_MAX_NODE_LIST_LEN (32)

#define HOMIE_MQTT_CONNECTED_BIT BIT0
#define HOMIE_MQTT_UPDATE_REQUIRED_BIT BIT1

#define QOS_1 (1)
#define RETAINED (1)

typedef struct {
    esp_mqtt_client_config_t mqtt_config;
    char device_name[HOMIE_MAX_DEVICE_NAME_LEN];
    char base_topic[HOMIE_MAX_BASE_TOPIC_LEN];
    char node_list[HOMIE_MAX_NODE_LIST_LEN];
    int stats_interval;
    bool loop;
    bool disable_publish_attributes;
    void (*connected_handler)();
    void (*msg_handler)(const char *, const char *);
} homie_config_t;

int homie_remove_retained(const char *topic);
void homie_init(homie_config_t *config);
void homie_subscribe(const char *subtopic, const int qos);
int homie_publish(const char *subtopic, int qos, int retain, const char *payload);
int homie_publishf(const char *subtopic, int qos, int retain, const char *format, ...);
int homie_publish_int(const char *subtopic, int qos, int retain, const int payload);
void homie_mktopic(char *topic, const char *subtopic);
void homie_disconnect(void);

extern volatile EventGroupHandle_t homie_event_group;

#endif
