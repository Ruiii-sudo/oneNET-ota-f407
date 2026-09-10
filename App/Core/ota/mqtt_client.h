/**
 ******************************************************************************
 * @file    mqtt_client.h
 * @brief   极简 MQTT 3.1.1 客户端（QoS0 为主，支持订阅/发布/心跳）
 *
 * 基于 ESP01S TCP 承载。服务端仅需一个支持 MQTT 3.1.1 的 Broker
 * （如 EMQX / Mosquitto / 阿里云物联网平台）。
 ******************************************************************************
 */
#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MQTT_OK        0
#define MQTT_ERR      -1
#define MQTT_TIMEOUT  -2
#define MQTT_DISCONNECTED -3
#define MQTT_AUTH_ERR -4   /* CONNACK return code != 0：clientId/用户名/token 鉴权失败 */

#define MQTT_MAX_TOPIC_LEN    96
#define MQTT_MAX_PAYLOAD_LEN  512
#define MQTT_MAX_CLIENT_ID    64
#define MQTT_MAX_USERNAME_LEN 48
#define MQTT_MAX_PASSWORD_LEN 160   /* OneNET token 字符串约 125 字符，必须足够大 */

typedef struct {
    char     host[64];
    uint16_t port;
    char     client_id[MQTT_MAX_CLIENT_ID];
    char     username[MQTT_MAX_USERNAME_LEN];  /* 可为空串 */
    char     password[MQTT_MAX_PASSWORD_LEN];  /* 可为空串 */
    uint16_t keepalive;         /* 秒，如 60 */
} mqtt_cfg_t;

/* 初始化（保存配置，不联网） */
void mqtt_init(const mqtt_cfg_t *cfg);

/* 建立 TCP 并完成 MQTT CONNECT/CONNACK 握手 */
int  mqtt_connect(uint32_t timeout_ms);

/* 订阅主题（QoS0），等待 SUBACK */
int  mqtt_subscribe(const char *topic, uint8_t qos, uint32_t timeout_ms);

/* 发布消息（QoS0） */
int  mqtt_publish(const char *topic, const uint8_t *payload, uint16_t len, uint32_t timeout_ms);

/* 发送 PINGREQ 并等待 PINGRESP */
int  mqtt_ping(uint32_t timeout_ms);

/*
 * 读取一个入站 MQTT 包（带超时）。
 * 若收到 PUBLISH，则调用 on_publish(topic, payload, len, arg)。
 * @retval 1 处理了一个包；0 超时；<0 错误/断开
 */
int  mqtt_loop(uint32_t timeout_ms,
               void (*on_publish)(const char *topic, const uint8_t *payload, uint16_t len, void *arg),
               void *arg);

/* 发送 DISCONNECT 并关闭 TCP */
void mqtt_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_CLIENT_H */
