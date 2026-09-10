/**
 ******************************************************************************
 * @file    mqtt_client.c
 * @brief   极简 MQTT 3.1.1 客户端实现
 ******************************************************************************
 */
#include "mqtt_client.h"
#include "esp01s.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

static mqtt_cfg_t s_cfg;
static uint16_t   s_pkt_id = 0;

/* ---- 小工具 ---- */

/* 写入 MQTT 剩余长度变长编码，返回编码字节数 */
static uint8_t mqtt_encode_len(uint8_t *out, uint32_t len)
{
    uint8_t n = 0;
    do
    {
        uint8_t b = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0)
        {
            b |= 0x80;
        }
        out[n++] = b;
    } while (len > 0);
    return n;
}

/* 从流中精确读取 n 字节 */
static int mqtt_read_exact(uint8_t *buf, uint32_t n, uint32_t timeout_ms)
{
    uint32_t got = 0;
    while (got < n)
    {
        int r = esp01s_read(buf + got, n - got, timeout_ms);
        if (r <= 0)
        {
            return (r < 0) ? r : MQTT_ERR;
        }
        got += (uint32_t)r;
    }
    return MQTT_OK;
}


/* 读取并丢弃 n 字节 */
static int mqtt_discard(uint32_t n, uint32_t timeout_ms)
{
    uint8_t tmp[64];
    while (n > 0)
    {
        uint32_t take = (n > sizeof(tmp)) ? sizeof(tmp) : n;
        int r = mqtt_read_exact(tmp, take, timeout_ms);
        if (r != MQTT_OK) return r;
        n -= take;
    }
    return MQTT_OK;
}

/* ---- 对外接口 ---- */

void mqtt_init(const mqtt_cfg_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(mqtt_cfg_t));
    s_pkt_id = 1;
}

int mqtt_connect(uint32_t timeout_ms)
{
    uint8_t pkt[256];   /* 容纳 OneNET 长 token：固定头10 + clientID 7 + user 12 + token 127 ≈ 156 */
    uint16_t pos = 0;
    uint16_t clen;
    uint8_t remain_len[4];
    uint8_t rl;
    uint8_t hdr[2];
    uint32_t body_len;
    uint8_t connack[4];
    int r;

    if (esp01s_tcp_connect(s_cfg.host, s_cfg.port, 5000) != ESP_OK)
    {
        /* [FIX-9] 建连失败后尝试收尾：清掉模块侧可能残留的
           CIPSTART 中间态，避免下一次 AT 握手被阻塞 */
        esp01s_tcp_close();
        esp01s_flush_rx();
        return MQTT_ERR;
    }

    /* ---- 组装 CONNECT 报文 ---- */
    pos = 0;
    /* 可变头：协议名 MQTT / 级别 4 / 连接标志 / keepalive */
    pkt[pos++] = 0x00; pkt[pos++] = 0x04;
    pkt[pos++] = 'M';  pkt[pos++] = 'Q';
    pkt[pos++] = 'T';  pkt[pos++] = 'T';
    pkt[pos++] = 0x04;

    {
        uint8_t flags = 0x02;   /* Clean Session */
        if (s_cfg.username[0] != '\0') flags |= 0x80;
        if (s_cfg.password[0] != '\0') flags |= 0x40;
        pkt[pos++] = flags;
    }

    pkt[pos++] = (uint8_t)(s_cfg.keepalive >> 8);
    pkt[pos++] = (uint8_t)(s_cfg.keepalive & 0xFF);

    /* ClientID（长度前缀） */
    clen = (uint16_t)strlen(s_cfg.client_id);
    pkt[pos++] = (uint8_t)(clen >> 8);
    pkt[pos++] = (uint8_t)(clen & 0xFF);
    memcpy(pkt + pos, s_cfg.client_id, clen);
    pos += clen;

    if (s_cfg.username[0] != '\0')
    {
        clen = (uint16_t)strlen(s_cfg.username);
        pkt[pos++] = (uint8_t)(clen >> 8);
        pkt[pos++] = (uint8_t)(clen & 0xFF);
        memcpy(pkt + pos, s_cfg.username, clen);
        pos += clen;
    }
    if (s_cfg.password[0] != '\0')
    {
        clen = (uint16_t)strlen(s_cfg.password);
        pkt[pos++] = (uint8_t)(clen >> 8);
        pkt[pos++] = (uint8_t)(clen & 0xFF);
        memcpy(pkt + pos, s_cfg.password, clen);
        pos += clen;
    }

    /* 固定头 */
    body_len = pos;
    rl = mqtt_encode_len(remain_len, body_len);
    hdr[0] = 0x10;   /* CONNECT */
    if (esp01s_send(hdr, 1, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(remain_len, rl, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(pkt, body_len, 2000) != ESP_OK) return MQTT_ERR;

    /* ---- 等待 CONNACK ---- */
    /* [FIX-26] CONNACK 前跳过杂讯：AT 2.3.0-dev 固件实测会在数据帧前混入
       OK/空行/CLOSED 等残留（CONNECT 的 "Recv 154 bytes SEND OK" 与 +IPD
       同批到达时，SEND OK 之后的残留字节会先被 mqtt_read_exact 读到）。
       旧实现读到杂讯 → hdr[0]&0xF0 != 0x20 → 立即判失败（02:08 监听 19ms
       快速失败实证）→ ota 层走失败路径 mqtt_disconnect() 发 DISCONNECT
       （0xE0 0x00，恰为 2 字节 → 日志 "Recv 2 bytes"）→ 服务器 CLOSED。
       这里最多跳过 8 字节杂讯，直到读到 0x20 0x02 的 CONNACK 头。 */
    {
        uint8_t found = 0;
        for (uint32_t skip = 0; skip < 8 && !found; skip++)
        {
            r = mqtt_read_exact(hdr, 1, timeout_ms);
            if (r != MQTT_OK)
            {
                esp01s_tcp_close();
                return MQTT_ERR;
            }
            if ((hdr[0] & 0xF0) != 0x20) continue;   /* 杂讯，丢弃继续 */
            r = mqtt_read_exact(hdr + 1, 1, timeout_ms);
            if (r != MQTT_OK)
            {
                esp01s_tcp_close();
                return MQTT_ERR;
            }
            if (hdr[1] == 2) found = 1;
            /* 0x20 开头但第二字节非 2：视为杂讯（如空格+其他）继续跳过 */
        }
        if (!found)
        {
            esp01s_tcp_close();
            return MQTT_ERR;
        }
    }

    r = mqtt_read_exact(connack, 2, timeout_ms);
    if (r != MQTT_OK)
    {
        esp01s_tcp_close();
        return MQTT_ERR;
    }
    /* [FIX-10] CONNACK return code 非 0：明确返回"鉴权失败"，
       OTA 层可据此区分"网络连不上"与"clientId/用户名/token 错误" */
    if (connack[1] != 0)
    {
        esp01s_tcp_close();
        return MQTT_AUTH_ERR;
    }

    return MQTT_OK;
}

int mqtt_subscribe(const char *topic, uint8_t qos, uint32_t timeout_ms)
{
    uint8_t pkt[192];
    uint16_t pos = 0;
    uint16_t tlen = (uint16_t)strlen(topic);
    uint8_t remain_len[4];
    uint8_t rl;
    uint8_t hdr[5];
    uint32_t body_len;
    uint32_t rl_len;
    int r;

    s_pkt_id++;

    pos = 0;
    pkt[pos++] = (uint8_t)(s_pkt_id >> 8);
    pkt[pos++] = (uint8_t)(s_pkt_id & 0xFF);
    pkt[pos++] = (uint8_t)(tlen >> 8);
    pkt[pos++] = (uint8_t)(tlen & 0xFF);
    memcpy(pkt + pos, topic, tlen);
    pos += tlen;
    pkt[pos++] = (qos & 0x03);

    body_len = pos;
    rl = mqtt_encode_len(remain_len, body_len);
    hdr[0] = 0x82;   /* SUBSCRIBE, QoS1 */

    if (esp01s_send(hdr, 1, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(remain_len, rl, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(pkt, body_len, 2000) != ESP_OK) return MQTT_ERR;

    /* ---- 等待 SUBACK (0x90) ---- */
    /* [FIX-26] SUBACK 前同样跳过杂讯（防御性，与 CONNACK 同因）：
       固件可能在 +IPD 前混入 OK/空行等，先读到则误判失败。 */
    {
        uint8_t found = 0;
        for (uint32_t skip = 0; skip < 8 && !found; skip++)
        {
            r = mqtt_read_exact(hdr, 1, timeout_ms);
            if (r != MQTT_OK) return MQTT_ERR;
            if ((hdr[0] & 0xF0) != 0x90) continue;   /* 杂讯，丢弃继续 */
            r = mqtt_read_exact(hdr + 1, 1, timeout_ms);
            if (r != MQTT_OK) return MQTT_ERR;
            if (hdr[1] <= 4) found = 1;   /* SUBACK 剩余长度：pid 2 + 返回码 1 */
        }
        if (!found) return MQTT_ERR;
    }
    rl_len = hdr[1];
    return mqtt_discard(rl_len, timeout_ms);
}

int mqtt_publish(const char *topic, const uint8_t *payload, uint16_t len, uint32_t timeout_ms)
{
    uint8_t pkt[768];
    uint16_t pos = 0;
    uint16_t tlen = (uint16_t)strlen(topic);
    uint8_t remain_len[4];
    uint8_t rl;
    uint8_t hdr[2];
    uint32_t body_len;

    if (sizeof(pkt) < (uint32_t)tlen + len + 4U)
    {
        return MQTT_ERR;
    }

    pos = 0;
    pkt[pos++] = (uint8_t)(tlen >> 8);
    pkt[pos++] = (uint8_t)(tlen & 0xFF);
    memcpy(pkt + pos, topic, tlen);
    pos += tlen;
    memcpy(pkt + pos, payload, len);
    pos += len;

    body_len = pos;
    rl = mqtt_encode_len(remain_len, body_len);
    hdr[0] = 0x30;   /* PUBLISH, QoS0 */

    if (esp01s_send(hdr, 1, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(remain_len, rl, 2000) != ESP_OK) return MQTT_ERR;
    if (esp01s_send(pkt, body_len, 2000) != ESP_OK) return MQTT_ERR;
    return MQTT_OK;
}

int mqtt_ping(uint32_t timeout_ms)
{
    uint8_t ping[2] = {0xC0, 0x00};
    uint8_t hdr[2];
    int r;

    if (esp01s_send(ping, 2, 2000) != ESP_OK) return MQTT_ERR;

    /* 等待 PINGRESP (0xD0) */
    r = mqtt_read_exact(hdr, 2, timeout_ms);
    if (r != MQTT_OK) return MQTT_ERR;
    if ((hdr[0] & 0xF0) != 0xD0) return MQTT_ERR;
    return MQTT_OK;
}

int mqtt_loop(uint32_t timeout_ms,
              void (*on_publish)(const char *topic, const uint8_t *payload, uint16_t len, void *arg),
              void *arg)
{
    uint32_t start = HAL_GetTick();
    uint8_t hdr[2];
    uint32_t body_len;
    int r;

    /* 等待包头（带超时） */
    while (1)
    {
        r = esp01s_read(hdr, 1, 100);
        if (r == 1)
        {
            break;
        }
        if (r == ESP_TIMEOUT)
        {
            if ((HAL_GetTick() - start) >= timeout_ms)
            {
                return 0;
            }
            continue;
        }
        return MQTT_DISCONNECTED;
    }

    r = mqtt_read_exact(hdr + 1, 1, timeout_ms);
    if (r != MQTT_OK) return r;
    body_len = hdr[1];
    if (hdr[1] & 0x80)
    {
        /* 多字节剩余长度：简化处理，直接读取并丢弃剩余 */
        uint32_t mult = 1;
        uint32_t value = (uint32_t)(hdr[1] & 0x7F);
        uint8_t b;
        for (uint8_t i = 0; i < 3; i++)
        {
            r = mqtt_read_exact(&b, 1, timeout_ms);
            if (r != MQTT_OK) return r;
            mult *= 128U;
            value += (uint32_t)(b & 0x7F) * mult;
            if ((b & 0x80) == 0)
            {
                body_len = value;
                break;
            }
        }
    }

    switch (hdr[0] & 0xF0)
    {
    case 0x30:   /* PUBLISH */
    {
        static uint8_t body[768];
        uint8_t topic[MQTT_MAX_TOPIC_LEN + 1];
        uint16_t tlen;
        uint32_t payload_off;

        if (body_len > sizeof(body))
        {
            return MQTT_ERR;
        }
        r = mqtt_read_exact(body, body_len, timeout_ms);
        if (r != MQTT_OK) return r;

        tlen = (uint16_t)((body[0] << 8) | body[1]);
        if (tlen > MQTT_MAX_TOPIC_LEN)
        {
            return MQTT_ERR;
        }
        memcpy(topic, body + 2, tlen);
        topic[tlen] = '\0';
        payload_off = 2U + tlen;

        /* QoS1/2 带报文 ID（2 字节），QoS0 不带 */
        {
            uint8_t qos = (uint8_t)((hdr[0] >> 1) & 0x03);
            if (qos > 0)
            {
                payload_off += 2U;
            }
        }

        if (on_publish != NULL)
        {
            on_publish((const char *)topic, body + payload_off,
                       (uint16_t)(body_len - payload_off), arg);
        }
        return 1;
    }
    case 0xD0:   /* PINGRESP */
        return 1;
    case 0x20:   /* CONNACK（异常重复到达，丢弃） */
    case 0x90:   /* SUBACK */
    case 0x40:   /* PUBACK */
    case 0x50:   /* PUBREC */
    case 0x70:   /* PUBCOMP */
        return mqtt_discard(body_len, timeout_ms) == MQTT_OK ? 1 : MQTT_ERR;
    default:
        return mqtt_discard(body_len, timeout_ms) == MQTT_OK ? 1 : MQTT_ERR;
    }
}

void mqtt_disconnect(void)
{
    uint8_t disc[2] = {0xE0, 0x00};
    esp01s_send(disc, 2, 1000);
    esp01s_tcp_close();
}
