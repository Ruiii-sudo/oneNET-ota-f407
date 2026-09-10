/**
 ******************************************************************************
 * @file    esp01s.c
 * @brief   ESP-01S WiFi 模块驱动实现（USART3 + AT + TCP 单连接）
 *
 * [FIX-4] 模块自恢复：AT 握手失败时自动 AT+RST 软复位后再握手，
 *         避免模块卡死（如上次 CIPSTART 超时残留中间态）后
 *         永久 "esp init fail, retry" 死循环。
 * [FIX-5] TCP 建连 DNS 兜底：老版 AT 固件（<1.0）对域名解析支持差，
 *         建连失败时按表回退到已知 IPv4（ESP8266 不支持 IPv6）。
 * [FIX-6] 每次发命令前先清空接收环形缓冲，防止上次应答残留
 *         （如 "SEND OK"）被误匹配为本次命令的 "OK"。
 ******************************************************************************
 */
#include "esp01s.h"
#include "main.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ---- 接收环形缓冲区 ---- */
static volatile uint8_t  s_ring[ESP_RX_RING_SIZE];
static volatile uint16_t s_ring_head = 0;   /* 写入位置 */
static volatile uint16_t s_ring_tail = 0;   /* 读取位置 */

/* ---- 剥离 +IPD 头后的字节流缓冲区 ---- */
static uint8_t  s_data[ESP_DATA_BUF_SIZE];
static volatile uint32_t s_data_len  = 0;
static volatile uint32_t s_data_head = 0;

/* ---- [FIX-24] USART3 RX DMA 循环接收 ----
   根因：flash_if_write 批量 4 字节编程（HAL_FLASH_Program 连续执行）期间
   F407 CPU 取指 stall，USART3 单字节接收中断完全无法进入，到达字节被
   ORE 覆盖丢失——OTA 下载 57600 下实测丢 ~7%（全量 179080 丢 13052，
   断点 166028），固件内容散布空洞 → MD5 必错（总量靠断点续传补齐）。
   DMA 硬件搬运不依赖 CPU 中断，flash 编程期间数据照常进缓冲，根治丢字节。
   本缓冲由 esp01s_parse() 轮询 NDTR 搬运到接收环形缓冲，无需 DMA 中断。 */
#define ESP_DMA_RX_SIZE 4096U
static uint8_t  s_dma_rx[ESP_DMA_RX_SIZE];
static volatile uint16_t s_dma_last = 0;   /* DMA 缓冲中已搬运到 ring 的位置 */

extern DMA_HandleTypeDef hdma_usart3_rx;   /* CubeMX: USART3_RX Circular */

/* 从 DMA 循环缓冲搬运新字节到接收环形缓冲（在 esp01s_parse 开头调用） */
static void esp01s_dma_to_ring(void)
{
    uint16_t cnt, cur;

    /* ORE 置位会停止 USART 接收，DMA 模式极少发生但必须清（读 SR 后写 0） */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(&huart3);
    }

    cnt = (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
    cur = (uint16_t)((ESP_DMA_RX_SIZE - cnt) % ESP_DMA_RX_SIZE);
    while (s_dma_last != cur)
    {
        esp01s_feed_byte(s_dma_rx[s_dma_last]);
        s_dma_last = (uint16_t)((s_dma_last + 1U) % ESP_DMA_RX_SIZE);
    }
}

/* IPD 解析状态 */
#define IPD_STATE_HEAD 0   /* 等待帧头 */
#define IPD_STATE_LEN  1   /* 解析长度数字 */
#define IPD_STATE_DATA 2   /* 搬运数据 */

static volatile uint8_t  s_ipd_state  = IPD_STATE_HEAD;
static volatile uint32_t s_ipd_len    = 0;
static volatile uint8_t  s_ipd_line[48];
static volatile uint8_t  s_ipd_line_n = 0;
static volatile uint8_t  s_closed     = 0;   /* 收到 CLOSED */
/* [FIX-24] 接收已改 DMA，单字节缓冲不再使用（s_rx_byte 已删除） */

/* ---- 内部函数 ---- */
static uint32_t ring_available(void);
static int  ring_pop(void);
static void esp01s_parse(void);
void esp01s_flush_rx(void);
static int  esp01s_soft_reset(void);

/* ---- 已知主机 -> IPv4 直连表（[FIX-17] IP 优先） ----
   老 AT 固件解析域名时，DNS 查询可能长时间挂起（尤其默认
   8.8.8.8 在国内不可达），期间 AT 引擎忙，任何命令（含 AT+RST）
   都不响应——这是“check fail 后 esp init fail 死循环”的根源。
   对已知主机直接使用 IP 建连（HTTP 请求头会带原始域名），
   彻底跳过模块 DNS。IP 经 Ali 公共 DNS(223.5.5.5) 复核；
   ip2 为备用地址。 */
static const struct {
    const char *host;
    const char *ip1;
    const char *ip2;   /* 备用 IP，可为 NULL */
} s_host_ip[] = {
    { "iot-api.heclouds.com",     "183.230.40.33",  NULL              },
    { "mqtts.heclouds.com",       "183.230.40.96",  NULL              },
    { "studio-mqtt.heclouds.com", "218.201.45.7",   "183.230.102.116" },
};

void esp01s_feed_byte(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_ring_head + 1U) % ESP_RX_RING_SIZE);
    if (next != s_ring_tail)
    {
        s_ring[s_ring_head] = byte;
        s_ring_head = next;
    }
    /* 环形缓冲满则丢弃（解析侧会尽快消费） */
}

static uint32_t ring_available(void)
{
    return (uint32_t)((uint16_t)(s_ring_head - s_ring_tail));
}

static int ring_pop(void)
{
    int b;
    if (s_ring_head == s_ring_tail)
    {
        return -1;
    }
    b = s_ring[s_ring_tail];
    s_ring_tail = (uint16_t)((s_ring_tail + 1U) % ESP_RX_RING_SIZE);
    return b;
}

/* [FIX-6] 丢弃接收环形缓冲中的全部残留字节 */
void esp01s_flush_rx(void)
{
    while (ring_available() > 0)
    {
        ring_pop();
    }
    s_data_len  = 0;
    s_data_head = 0;
    /* [FIX-24] DMA 消费位置同步到当前写指针（丢弃 DMA 缓冲残留） */
    {
        uint16_t cnt = (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
        s_dma_last = (uint16_t)((ESP_DMA_RX_SIZE - cnt) % ESP_DMA_RX_SIZE);
    }
}

/* ================= +IPD 帧解析 ================= */

/**
 * @brief 从环形缓冲解析字节到数据流缓冲（剥离 +IPD,<len>: 帧头）
 *        同时识别 "CLOSED"/"Unlink" 连接断开提示。
 */
static void esp01s_parse(void)
{
    esp01s_dma_to_ring();   /* [FIX-24] 先把 DMA 新字节搬入接收环形缓冲 */

    while (ring_available() > 0)
    {
        int b = ring_pop();
        if (b < 0) break;

        if (s_ipd_state == IPD_STATE_HEAD)
        {
            /* 只有一帧开始处的 '+' 才可能是 IPD 帧头 */
            if (b == '+' && s_ipd_line_n == 0)
            {
                s_ipd_line[0] = '+';
                s_ipd_line_n  = 1;
                s_ipd_state   = IPD_STATE_LEN;
            }
            else if (b == '\r' || b == '\n')
            {
                /* [FIX-26] +IPD 帧间的 \r\n 分隔符直接丢弃，不推入 s_data。
                   AT 2.3.0-dev 在 +IPD 帧之间会发 \r\n\r\n（帧尾标记），旧
                   逻辑把这些空白字节当成 body 数据推入 s_data，被
                   http_client 读走写 flash，导致 MD5 错（实测在 body 偏移
                   0x0A07 处插入 4 字节 \r\n\r\n，末尾 4 字节被挤出）。
                   body 数据在 DATA 态搬运，DATA 态不丢弃任何字节，不受影响。 */
            }
            else if (b == '+' && s_ipd_line_n > 0)
            {
                /* 上一行数据残留：视为数据 */
                if (s_data_len < ESP_DATA_BUF_SIZE)
                {
                    s_data[(s_data_head + s_data_len) % ESP_DATA_BUF_SIZE] = (uint8_t)b;
                    s_data_len++;
                }
            }
            else
            {
                /* 非 '+' 开头：AT 提示行或直接数据 */
                if (s_data_len < ESP_DATA_BUF_SIZE)
                {
                    s_data[(s_data_head + s_data_len) % ESP_DATA_BUF_SIZE] = (uint8_t)b;
                    s_data_len++;
                }
            }
        }
        else if (s_ipd_state == IPD_STATE_LEN)
        {
            /* 累积行，直到 ':' 或 '\n' */
            if (s_ipd_line_n < (uint8_t)sizeof(s_ipd_line) - 1U)
            {
                s_ipd_line[s_ipd_line_n++] = (uint8_t)b;
            }
            if (b == '\n')
            {
                /* 一整行结束，但没等到 ':'——可能是 "CLOSED"/"Unlink"/"OK" 等提示 */
                if (strstr((const char *)s_ipd_line, "CLOSED") != NULL ||
                    strstr((const char *)s_ipd_line, "Unlink") != NULL)
                {
                    s_closed = 1;
                }
                s_ipd_line_n = 0;
                s_ipd_state  = IPD_STATE_HEAD;
            }
            else if (b == ':')
            {
                /* 提取 +IPD,<len>: 中的长度 */
                const char *p = strchr((const char *)s_ipd_line, ',');
                uint32_t len = 0;
                if (p != NULL)
                {
                    p++;
                    while (*p >= '0' && *p <= '9')
                    {
                        len = len * 10U + (uint32_t)(*p - '0');
                        p++;
                    }
                }
                s_ipd_len   = len;
                s_ipd_state = IPD_STATE_DATA;
                s_ipd_line_n = 0;
            }
        }
        else /* IPD_STATE_DATA */
        {
            /* 搬运数据字节 */
            if (s_ipd_len > 0)
            {
                if (s_data_len < ESP_DATA_BUF_SIZE)
                {
                    s_data[(s_data_head + s_data_len) % ESP_DATA_BUF_SIZE] = (uint8_t)b;
                    s_data_len++;
                }
                s_ipd_len--;
                if (s_ipd_len == 0)
                {
                    s_ipd_state = IPD_STATE_HEAD;
                }
            }
            else
            {
                s_ipd_state = IPD_STATE_HEAD;
            }
        }
    }
}

/* ================= 对外接口 ================= */

/**
 * [FIX-4] AT 软复位：AT+RST 后等待 "ready"（老固件也可能只回 OK 或
 * 无回显，因此结果不作致命处理，随后仍会做正式握手校验）。
 */
static int esp01s_soft_reset(void)
{
    esp01s_flush_rx();
    (void)esp01s_at_cmd("AT+RST\r\n", "ready", 4000);
    /* 模块冷启动需要约 1~2s（内部上电自检 + WiFi 初始化） */
    HAL_Delay(1500);
    esp01s_flush_rx();
    return ESP_OK;
}

int esp01s_init(void)
{
    int i;
    int ok = 0;

    s_ring_head = 0;
    s_ring_tail = 0;
    s_data_len  = 0;
    s_data_head = 0;
    s_ipd_state = IPD_STATE_HEAD;
    s_ipd_len   = 0;
    s_ipd_line_n = 0;
    s_closed    = 0;

    /* [FIX-1][FIX-24] 必须先启动 USART3 RX DMA 循环接收，再发 AT 命令：
       （原 FIX-1 的单字节中断在 flash 编程期间被 CPU stall 阻塞会 ORE
       丢字节，已整体改 DMA）DMA 硬件搬运不受 flash 编程影响。 */
    if (HAL_UART_Receive_DMA(&huart3, s_dma_rx, ESP_DMA_RX_SIZE) != HAL_OK)
    {
        /* [FIX-18] 接收状态异常（HAL_BUSY，此前日志重试轮 init
           13ms 内秒败的根源）：先 Abort 复位接收状态，重试一次再失败 */
        (void)HAL_UART_AbortReceive(&huart3);
        HAL_Delay(10);
        if (HAL_UART_Receive_DMA(&huart3, s_dma_rx, ESP_DMA_RX_SIZE) != HAL_OK)
        {
            return ESP_ERR;
        }
    }

    /* [FIX-2] AT 命令必须带 "\r\n" 行结束符，ESP-AT 固件按行解析，
       无结束符的命令不会执行也不会回复。 */
    /* 先快速握手（模块健康时无需等待 RST 复位时间） */
    for (i = 0; i < 2; i++)
    {
        if (esp01s_at_cmd("AT\r\n", "OK", 1500) == ESP_OK)
        {
            ok = 1;
            break;
        }
    }

    /* [FIX-4] 握手失败：模块可能处于卡死/中间态（例如上次
       AT+CIPSTART 超时未收尾），先尽力收尾 + 软复位，再长时探测：
       模块内部 TCP/DNS 重试最长约 15~30s，期间引擎忙不响应任何
       命令，必须等它恢复空闲（最长 30s 恢复窗口）。 */
    if (!ok)
    {
        esp01s_at_cmd("AT+CIPCLOSE\r\n", "OK", 800);   /* 尽力收尾残留连接 */
        esp01s_soft_reset();
        for (i = 0; i < 15; i++)    /* 15 × 2s = 最长 30s */
        {
            if (esp01s_at_cmd("AT\r\n", "OK", 2000) == ESP_OK)
            {
                ok = 1;
                break;
            }
        }
        if (!ok)
        {
            return ESP_TIMEOUT;
        }
    }

    esp01s_at_cmd("ATE0\r\n", "OK", 1000);        /* 关闭回显（失败不致命） */
    esp01s_at_cmd("AT+CIPMUX=0\r\n", "OK", 1000); /* 单连接模式 */
    esp01s_at_cmd("AT+CIPMODE=0\r\n", "OK", 1000);/* 非透传模式 */
    /* [FIX-18] 已删除 AT+CIPDNS_DEF：实测本固件（AT 2.3.0-dev /
       Bin 2.2.0）返回 ERROR，且紧接着触发 WIFI DISCONNECT（两次
       串口监听均复现），重连后的 TCP 为半死连接（CONNECT OK 但
       SEND OK 后服务器零响应即 CLOSED）。IP 直连不依赖模块 DNS，
       此命令无意义且有害，直接移除。 */

    return ESP_OK;
}


/**
 * @brief USART3 接收完成回调（强定义，覆盖 HAL 弱定义）
 * @note  [FIX-24] 接收已改 USART3 RX DMA 循环，单字节中断不再使用；
 *        本回调留空占位，防止 HAL 弱定义在别处被误接。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    (void)huart;
}


/**
 * @brief 发送 AT 命令并等待期望串
 * @note  响应与 AT 数据共用同一环形缓冲；期间若夹杂 +IPD 数据也会被解析入流缓冲
 *        [FIX-3] esp01s_parse() 会把环形缓冲中的全部应答字节（含 AT 响应
 *        "OK"/"ERROR" 等）搬进 s_data 流缓冲，因此必须从 s_data 匹配期望串，
 *        而不能直接从环形缓冲取（环形缓冲在 parse 之后已为空）。
 */
int esp01s_at_cmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    char collect[128];
    uint32_t n = 0;

    /* [FIX-6] 丢弃上次命令残留的应答字节（含环形缓冲），避免误匹配 */
    esp01s_flush_rx();

    /* 发送命令 */
    if (HAL_UART_Transmit(&huart3, (uint8_t *)cmd, (uint16_t)strlen(cmd), 500) != HAL_OK)
    {
        return ESP_ERR;
    }

    /* 等待期望串 / 失败提示 / 超时 */
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        esp01s_parse();
        esp01s_wait_hook();
        while (s_data_len > 0)
        {
            uint8_t b = s_data[s_data_head];
            s_data_head = (s_data_head + 1U) % ESP_DATA_BUF_SIZE;
            s_data_len--;
            if (n < sizeof(collect) - 1U)
            {
                collect[n++] = (char)b;
                collect[n] = '\0';
            }
            /* 滚动匹配 */
            if (strstr(collect, expect) != NULL)
            {
                return ESP_OK;
            }
            /* [FIX-7] 补充识别常见失败/非预期提示，避免干等超时：
               "DNS Fail" / "CONNECT FAIL" / "ALREADY CONNECTED" */
            if (strstr(collect, "ERROR")    != NULL ||
                strstr(collect, "FAIL")     != NULL ||
                strstr(collect, "DNS Fail") != NULL ||
                strstr(collect, "ALREADY CONNECTED") != NULL)
            {
                return ESP_ERR;
            }
        }
        HAL_Delay(2);
    }
    return ESP_TIMEOUT;
}

int esp01s_join_ap(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pass);
    return esp01s_at_cmd(cmd, "OK", timeout_ms);
}

/* [FIX-20] 查询 WiFi 连接状态：AT+CWJAP?，+CWJAP:3 表示已连接并获取到 IP
 * （AT 2.x 应答示例：+CWJAP:3\r\nOK\r\n；未连接回 +CWJAP:0 或 ERROR）。
 * 注意：与 esp01s_at_cmd 不同，这里等待 "+CWJAP:3" 而非 "OK"，
 * 因为未连接时也会回 OK，必须解析状态数字才能区分。 */
int esp01s_wifi_status(uint32_t timeout_ms)
{
    char collect[96];
    uint32_t n = 0;
    uint32_t start = HAL_GetTick();

    /* 先丢弃残留应答，避免旧数据误判 */
    esp01s_flush_rx();

    if (HAL_UART_Transmit(&huart3, (uint8_t *)"AT+CWJAP?\r\n", 11, 500) != HAL_OK)
    {
        return ESP_ERR;
    }

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        esp01s_parse();
        esp01s_wait_hook();
        while (s_data_len > 0)
        {
            uint8_t b = s_data[s_data_head];
            s_data_head = (s_data_head + 1U) % ESP_DATA_BUF_SIZE;
            s_data_len--;
            if (n < sizeof(collect) - 1U)
            {
                collect[n++] = (char)b;
                collect[n] = '\0';
            }
            /* 已连接并获取 IP：+CWJAP:3（兼容无 + 前缀的固件格式） */
            if (strstr(collect, "CWJAP:3") != NULL)
            {
                return ESP_OK;
            }
            /* 明确未连接（+CWJAP:0/1/2/4）或模块报错 */
            if (strstr(collect, "CWJAP:0") != NULL ||
                strstr(collect, "CWJAP:1") != NULL ||
                strstr(collect, "CWJAP:2") != NULL ||
                strstr(collect, "CWJAP:4") != NULL ||
                strstr(collect, "ERROR") != NULL ||
                strstr(collect, "FAIL") != NULL)
            {
                return ESP_ERR;
            }
        }
        HAL_Delay(2);
    }
    return ESP_TIMEOUT;
}

/**
 * [FIX-15] 关键修复：CIPSTART 的期望串不能是 "CONNECT"——
 *   strstr("CONNECT FAIL", "CONNECT") 会命中，导致建连实际失败
 *   （DNS 失败/网络不通）被误判为成功，后续发 MQTT/HTTP 报文
 *   时卡在等 ">" 直到超时，最终表现为 "mqtt fail"。
 *   改用 "OK"（成功响应为 CONNECT\r\nOK\r\n；失败响应不含 OK，
 *   且被下方 FAIL/DNS Fail/ALREADY CONNECTED 捕获）。
 * [FIX-17] 已知主机 IP 优先直连：跳过模块 DNS，避免 DNS 挂起
 *   导致 AT 引擎长时间忙（期间命令全部不响应）；失败后排干
 *   挂起的终端响应，让引擎恢复空闲。
 */
static void esp01s_drain_pending(uint32_t max_ms)
{
    uint32_t start = HAL_GetTick();
    char collect[96];
    uint32_t n = 0;

    while ((HAL_GetTick() - start) < max_ms)
    {
        esp01s_parse();
        esp01s_wait_hook();
        while (s_data_len > 0)
        {
            uint8_t b = s_data[s_data_head];
            s_data_head = (s_data_head + 1U) % ESP_DATA_BUF_SIZE;
            s_data_len--;
            if (n < sizeof(collect) - 1U)
            {
                collect[n++] = (char)b;
                collect[n] = '\0';
            }
            if (strstr(collect, "CONNECT FAIL") != NULL ||
                strstr(collect, "DNS Fail")    != NULL ||
                strstr(collect, "ERROR")       != NULL ||
                strstr(collect, "CLOSED")      != NULL ||
                strstr(collect, "Unlink")      != NULL ||
                strstr(collect, "OK")          != NULL)
            {
                return;   /* 引擎已吐出终态响应，恢复空闲 */
            }
        }
        HAL_Delay(2);
    }
}

int esp01s_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char cmd[128];
    uint32_t i;
    int k;
    int r;

    /* 已知主机：IP 直连优先，表内 IP 全失败后再退回域名（模块 DNS） */
    for (i = 0; i < sizeof(s_host_ip) / sizeof(s_host_ip[0]); i++)
    {
        if (strcmp(host, s_host_ip[i].host) == 0)
        {
            const char *ips[2];
            int n = 0;

            ips[n++] = s_host_ip[i].ip1;
            if (s_host_ip[i].ip2 != NULL)
            {
                ips[n++] = s_host_ip[i].ip2;
            }
            for (k = 0; k < n; k++)
            {
                snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
                         ips[k], (unsigned)port);
                r = esp01s_at_cmd(cmd, "OK", timeout_ms);
                if (r == ESP_OK)
                {
                    return ESP_OK;
                }
                esp01s_drain_pending(4000);   /* 排干挂起响应，引擎复位 */
            }
            /* 最后用域名试一次（模块 DNS 正常时仍可成功） */
            snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
                     host, (unsigned)port);
            r = esp01s_at_cmd(cmd, "OK", timeout_ms);
            esp01s_drain_pending(4000);
            return r;
        }
    }

    /* 未知主机：域名优先（维持原逻辑） */
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", host, (unsigned)port);
    r = esp01s_at_cmd(cmd, "OK", timeout_ms);
    if (r != ESP_OK)
    {
        esp01s_drain_pending(4000);
    }
    return r;
}

void esp01s_tcp_close(void)
{
    /* [FIX-23] AT 2.3.0-dev 对已关闭连接的 CIPCLOSE 不应答或回 ERROR：
       等 "OK" 1500ms 只会白等（v8 日志 CIPCLOSE 间隔 1.5s 实证）。
       at_cmd 遇到 ERROR 会立即返回，这里 300ms 仅作兜底。 */
    esp01s_at_cmd("AT+CIPCLOSE\r\n", "OK", 300);
}

/* ================= 只扫描不消费的 s_data 查找 =================
 * [FIX-21] 服务器响应（+IPD 数据）与 AT 应答（"SEND OK"/"CLOSED"）可能
 * 在同一批 parse 中进入 s_data。旧实现把字节全部消费进 collect 匹配，
 * 若响应先到会被吞掉 → HTTP 读不到响应 → 表现为"连不上服务器"。
 * 这两个函数只在环形缓冲中查找/丢弃指定标记，不动其它数据。 */

/* 在 s_data 中查找子串（不消费），返回子串结束位置的偏移；找不到返回 -1 */
static int32_t esp_data_find(const char *pat, uint32_t pat_len)
{
    uint32_t avail = s_data_len;
    if (pat_len == 0 || avail < pat_len)
    {
        return -1;
    }
    for (uint32_t i = 0; i + pat_len <= avail; i++)
    {
        uint32_t h = s_data_head + i;
        uint32_t k;
        for (k = 0; k < pat_len; k++)
        {
            if (s_data[(h + k) % ESP_DATA_BUF_SIZE] != (uint8_t)pat[k])
            {
                break;
            }
        }
        if (k == pat_len)
        {
            return (int32_t)(i + pat_len);
        }
    }
    return -1;
}

/* 从 s_data 头部丢弃 nbytes 字节 */
static void esp_data_drop(uint32_t nbytes)
{
    if (nbytes > s_data_len)
    {
        nbytes = s_data_len;
    }
    s_data_head = (s_data_head + nbytes) % ESP_DATA_BUF_SIZE;
    s_data_len -= nbytes;
}

/* 丢弃到标记结束位置（含），并顺带吃掉紧随的 "\r\n"（若存在） */
static void esp_data_drop_upto(int32_t end_off)
{
    uint32_t drop = (uint32_t)end_off;
    uint32_t h = (s_data_head + drop) % ESP_DATA_BUF_SIZE;
    /* 兼容 "SEND OK\r\n" / "CLOSED\r\n" 的换行符 */
    if (drop < s_data_len && s_data[h] == '\r')
    {
        drop++;
        h = (s_data_head + drop) % ESP_DATA_BUF_SIZE;
        if (drop < s_data_len && s_data[h] == '\n')
        {
            drop++;
        }
    }
    else if (drop < s_data_len && s_data[h] == '\n')
    {
        drop++;
    }
    esp_data_drop(drop);
}

/* 从 s_data 删除 [off, off+len) 区间的字节（环形缓冲中间段删除） */
static void esp_data_remove(uint32_t off, uint32_t len)
{
    uint32_t tail;
    if (off >= s_data_len || len == 0)
    {
        return;
    }
    if (off + len > s_data_len)
    {
        len = s_data_len - off;
    }
    tail = s_data_len - off - len;
    for (uint32_t i = 0; i < tail; i++)
    {
        s_data[(s_data_head + off + i) % ESP_DATA_BUF_SIZE] =
            s_data[(s_data_head + off + len + i) % ESP_DATA_BUF_SIZE];
    }
    s_data_len -= len;
}

int esp01s_send(const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    char cmd[32];
    uint32_t start = HAL_GetTick();
    char collect[64];
    uint32_t n = 0;
    uint8_t  error_seen = 0;   /* [FIX-22] 已收到 ERROR 标记 */
    uint32_t err_start  = 0;   /* [FIX-22] ERROR 到达时刻 */

    /* [FIX-3] 丢弃上次残留应答字节；">" 提示与 "SEND OK" 同样从 s_data 匹配 */
    esp01s_flush_rx();

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%lu\r\n", (unsigned long)len);
    if (HAL_UART_Transmit(&huart3, (uint8_t *)cmd, (uint16_t)strlen(cmd), 500) != HAL_OK)
    {
        return ESP_ERR;
    }

    /* 等待 ">" 提示符 */
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        esp01s_parse();
        esp01s_wait_hook();
        while (s_data_len > 0)
        {
            uint8_t b = s_data[s_data_head];
            s_data_head = (s_data_head + 1U) % ESP_DATA_BUF_SIZE;
            s_data_len--;
            if (n < sizeof(collect) - 1U)
            {
                collect[n++] = (char)b;
                collect[n] = '\0';
            }
            if (strstr(collect, ">") != NULL)
            {
                goto prompt_ok;
            }
            if (strstr(collect, "ERROR") != NULL)
            {
                return ESP_ERR;
            }
        }
        HAL_Delay(2);
    }
    return ESP_TIMEOUT;

prompt_ok:
    /* 发送数据 */
    if (HAL_UART_Transmit(&huart3, (uint8_t *)data, (uint16_t)len, 2000) != HAL_OK)
    {
        return ESP_ERR;
    }

    /* 等待 "SEND OK" / "CLOSED" / "ERROR"：
     * [FIX-21] 只扫描 s_data 不消费，避免吞掉与 SEND OK 同批到达的 +IPD 响应。
     * [FIX-22] AT 2.3.0-dev 开发版固件（v6/v7 监听实证）：服务器快速响应
     * 并关闭连接时，CIPSEND 后回 "ERROR" 而非 "SEND OK"。因此 ERROR
     * 不立即判失败：删除该标记（前后数据保留）后继续扫描，若随后出现
     * +IPD 数据或 CLOSED（请求已送达、响应已返回）则视为发送成功；
     * 800ms 宽限期内仍无任何数据才判定为真失败。 */
    start = HAL_GetTick();
    error_seen = 0;
    err_start = 0;
    for (;;)
    {
        esp01s_parse();
        esp01s_wait_hook();

        if (s_data_len > 0)
        {
            int32_t off;

            off = esp_data_find("SEND OK", 7);
            if (off >= 0)
            {
                esp_data_drop_upto(off);
                return ESP_OK;
            }
            off = esp_data_find("CLOSED", 6);
            if (off >= 0)
            {
                return ESP_OK;
            }
            if (!error_seen)
            {
                off = esp_data_find("ERROR", 5);
                if (off >= 0)
                {
                    uint32_t start_off = (uint32_t)off - 5U;
                    esp_data_remove(start_off, 5U);
                    /* 吃掉 ERROR 后的换行（如 "ERROR\r\n"） */
                    if (start_off < s_data_len &&
                        s_data[(s_data_head + start_off) % ESP_DATA_BUF_SIZE] == '\r')
                    {
                        esp_data_remove(start_off, 1U);
                        if (start_off < s_data_len &&
                            s_data[(s_data_head + start_off) % ESP_DATA_BUF_SIZE] == '\n')
                        {
                            esp_data_remove(start_off, 1U);
                        }
                    }
                    else if (start_off < s_data_len &&
                             s_data[(s_data_head + start_off) % ESP_DATA_BUF_SIZE] == '\n')
                    {
                        esp_data_remove(start_off, 1U);
                    }
                    error_seen = 1;
                    err_start = HAL_GetTick();
                }
            }
        }

        if (error_seen)
        {
            /* ERROR 后收到数据（+IPD 服务器响应）→ 发送实际成功 */
            if (s_data_len > 0)
            {
                return ESP_OK;
            }
            if ((HAL_GetTick() - err_start) > 800U)
            {
                return ESP_ERR;
            }
        }

        if ((HAL_GetTick() - start) >= timeout_ms)
        {
            return ESP_TIMEOUT;
        }
        HAL_Delay(2);
    }
}

int esp01s_read(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint32_t got = 0;

    while (got < len)
    {
        esp01s_parse();

        /* 从数据流缓冲搬取 */
        if (s_data_len > 0)
        {
            uint32_t take = s_data_len;
            if (take > (len - got))
            {
                take = len - got;
            }
            for (uint32_t i = 0; i < take; i++)
            {
                buf[got + i] = s_data[(s_data_head + i) % ESP_DATA_BUF_SIZE];
            }
            s_data_head = (s_data_head + take) % ESP_DATA_BUF_SIZE;
            s_data_len -= take;
            got += take;
            continue;
        }

        if (s_closed)
        {
            return (got > 0) ? (int)got : ESP_CLOSED;
        }

        if ((HAL_GetTick() - start) >= timeout_ms)
        {
            return (got > 0) ? (int)got : ESP_TIMEOUT;
        }
        esp01s_wait_hook();
        HAL_Delay(1);
    }
    return (int)got;
}


/* ================= 阻塞等待钩子 ================= */

/* 弱定义：OTA 任务可通过强定义实现喂狗，避免长阻塞时 IWDG 复位 */
__weak void esp01s_wait_hook(void)
{
}
