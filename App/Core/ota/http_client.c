/**
 ******************************************************************************
 * @file    http_client.c
 * @brief   极简 HTTP/1.1 客户端实现（ESP01S 承载，逐行解析）
 ******************************************************************************
 */
#include "http_client.h"
#include "esp01s.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 逐字节读取一行（到 \n 结束，剔除 \r） */
static int http_read_line(char *buf, uint32_t maxlen, uint32_t timeout_ms)
{
    uint32_t n = 0;
    uint32_t start = HAL_GetTick();
    while (n < maxlen - 1U)
    {
        uint8_t b;
        int r = esp01s_read(&b, 1, 100);
        if (r == 1)
        {
            if (b == '\n')
            {
                if (n > 0 && buf[n - 1] == '\r')
                {
                    n--;   /* 去掉 \r */
                }
                buf[n] = '\0';
                return (int)n;
            }
            buf[n++] = (char)b;
        }
        else if (r == ESP_TIMEOUT)
        {
            if ((HAL_GetTick() - start) >= timeout_ms)
            {
                return ESP_TIMEOUT;
            }
        }
        else
        {
            return r;
        }
    }
    return ESP_TIMEOUT;
}

/* 读取 n 字节到回调/缓冲区，返回实际处理情况 */
static int http_read_body(uint8_t *buf, uint32_t want, uint32_t timeout_ms,
                          void (*on_data)(const uint8_t *data, uint32_t len, void *arg),
                          void *arg, http_resp_t *resp)
{
    int r = esp01s_read(buf, want, timeout_ms);
    if (r <= 0)
    {
        return r;
    }
    if (on_data != NULL)
    {
        on_data(buf, (uint32_t)r, arg);
    }
    else if (resp->body != NULL)
    {
        uint32_t cap = resp->body_cap - resp->body_len;
        uint32_t copy = ((uint32_t)r < cap) ? (uint32_t)r : cap;
        memcpy(resp->body + resp->body_len, buf, copy);
        resp->body_len += copy;
    }
    return r;
}

/*
 * 解析一行的 chunk 大小（十六进制，忽略 ";参数" 后缀）
 */
static int http_parse_chunk_size(const char *line, uint32_t *size)
{
    uint32_t v = 0;
    const char *p = line;
    if (*p == '\0')
    {
        return -1;
    }
    while (*p && *p != ';' && *p != ' ' && *p != '\t')
    {
        char c = *p;
        uint32_t d;
        if (c >= '0' && c <= '9')
        {
            d = (uint32_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = (uint32_t)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            d = (uint32_t)(c - 'A' + 10);
        }
        else
        {
            return -1;
        }
        v = (v << 4) | d;
        if (v > 0xFFFFFFU)
        {
            return -1;
        }
        p++;
    }
    *size = v;
    return 0;
}

static int http_client_request_once(const char *host, uint16_t port,
                                    const char *method, const char *path,
                                    const char *extra_hdr, const char *body,
                                    http_resp_t *resp,
                                    void (*on_data)(const uint8_t *data, uint32_t len, void *arg),
                                    void *arg, uint32_t timeout_ms)
{
    char req[512];
    char line[160];
    uint32_t remaining;
    uint32_t want;
    uint8_t chunk[1024];
    int r;
    int chunked = 0;
    int keep_body = (on_data != NULL) || (resp->body != NULL);
    /* 调用方（ota.c）预置了 resp->body/body_cap 指向本地缓冲；
       memset 会把这些指针清成 NULL，导致响应体读出来后无处存放、
       body_len 恒为 0（check/status 的 JSON 解析永远失败）。
       先在 memset 前保存，再恢复。 */
    uint8_t *save_body      = resp->body;
    uint32_t save_body_cap  = resp->body_cap;
    volatile int *save_abort = resp->abort_flag;   /* 同 body 一并恢复 */

    memset(resp, 0, sizeof(http_resp_t));
    resp->status_code = -1;
    resp->ota_errno   = -1;
    resp->body        = save_body;
    resp->body_cap    = save_body_cap;
    resp->abort_flag  = save_abort;   /* 之前被 memset 清成 NULL，
                                         进度上报截断（每 10% 断连上报）从未生效 */

    /* 1. TCP 连接 */
    if (esp01s_tcp_connect(host, port, 5000) != ESP_OK)
    {
        /*  建连失败也收尾一次：避免模块残留 TCP 状态
           （AT 引擎忙）导致下次 init/建连卡死 */
        esp01s_tcp_close();
        return ESP_ERR;
    }

    /* 2. 构造请求：有 body 时头 + body 合并为一次 CIPSEND，
       减少发送阶段的 AT 往返与"服务器响应抢在 SEND OK 前到达"的竞态窗口 */
    if (body != NULL && body[0] != '\0')
    {
        int hl = snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Length: %u\r\n"
                 "%s"
                 "Connection: close\r\n"
                 "\r\n",
                 method, path, host, (unsigned)strlen(body),
                 (extra_hdr != NULL) ? extra_hdr : "");
        if (hl < 0 || (uint32_t)hl + strlen(body) >= sizeof(req))
        {
            esp01s_tcp_close();
            return ESP_ERR;
        }
        memcpy(req + hl, body, strlen(body) + 1U);   /* 连 '\0' 一起拷入 */
    }
    else if (extra_hdr != NULL)
    {
        snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "%s"
                 "Connection: close\r\n"
                 "\r\n",
                 method, path, host, extra_hdr);
    }
    else
    {
        snprintf(req, sizeof(req),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 method, path, host);
    }
    if (esp01s_send((const uint8_t *)req, (uint32_t)strlen(req), 3000) != ESP_OK)
    {
        esp01s_tcp_close();
        return ESP_ERR;
    }

    /* 3. 状态行：HTTP/1.1 200 OK / 206 Partial Content
        容错：AT 固件可能在响应前混入杂讯行
       （CLOSED/ERROR/空行等，v7/v8 监听实证），最多跳过 8 行 */
    r = http_read_line(line, sizeof(line), 3000);
    if (r < 0)
    {
        esp01s_tcp_close();
        return r;
    }
    for (int skip = 0; skip < 8 &&
         strncmp(line, "HTTP/1.1 ", 9) != 0; skip++)
    {
        r = http_read_line(line, sizeof(line), 3000);
        if (r < 0)
        {
            esp01s_tcp_close();
            return r;
        }
    }
    if (strncmp(line, "HTTP/1.1 ", 9) == 0)
    {
        resp->status_code = atoi(line + 9);
    }
    else
    {
        esp01s_tcp_close();
        return ESP_ERR;
    }

    /* 4. 响应头：直到空行 */
    while (1)
    {
        r = http_read_line(line, sizeof(line), 3000);
        if (r < 0)
        {
            esp01s_tcp_close();
            return r;
        }
        if (r == 0)
        {
            break;   /* 空行：头结束 */
        }
        if (strncmp(line, "Content-Length:", 15) == 0)
        {
            resp->content_len = (uint32_t)strtoul(line + 15, NULL, 10);
        }
        else if (strncmp(line, "Content-Range:", 14) == 0)
        {
            /* 形如 bytes 1024-190399/190400 */
            char *p = strchr(line + 14, ' ');
            if (p != NULL)
            {
                p++;
                resp->start_offset = (uint32_t)strtoul(p, &p, 10);
                if (*p == '-') p++;
                (void)strtoul(p, &p, 10);   /* end */
                if (*p == '/') p++;
                resp->total_size = (uint32_t)strtoul(p, NULL, 10);
            }
        }
        else if (strncmp(line, "Transfer-Encoding:", 18) == 0)
        {
            if (strstr(line + 18, "chunked") != NULL)
            {
                chunked = 1;
            }
        }
        else if (strncmp(line, "Ota-Errno:", 10) == 0)
        {
            resp->ota_errno = atoi(line + 10);
        }
    }

    /* 5. 读取正文 */
    if (chunked)
    {
        /* chunked 解码：循环读"大小行 + 数据 + CRLF"，直至 0 分块与尾随头 */
        for (;;)
        {
            uint32_t csize;

            if (resp->abort_flag != NULL && *resp->abort_flag != 0)
            {
                break;
            }
            r = http_read_line(line, sizeof(line), timeout_ms);
            if (r < 0)
            {
                esp01s_tcp_close();
                return r;
            }
            if (http_parse_chunk_size(line, &csize) != 0)
            {
                esp01s_tcp_close();
                return ESP_ERR;
            }
            if (csize == 0)
            {
                /* 尾随头直至空行 */
                while (1)
                {
                    r = http_read_line(line, sizeof(line), timeout_ms);
                    if (r < 0)
                    {
                        esp01s_tcp_close();
                        return r;
                    }
                    if (r == 0)
                    {
                        break;
                    }
                }
                break;
            }
            remaining = csize;
            while (remaining > 0)
            {
                if (resp->abort_flag != NULL && *resp->abort_flag != 0)
                {
                    break;
                }
                want = (remaining > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;
                r = http_read_body(chunk, want, timeout_ms, on_data, arg, resp);
                if (r <= 0)
                {
                    esp01s_tcp_close();
                    return (r < 0) ? r : ESP_ERR;
                }
                remaining -= (uint32_t)r;
            }
            if (remaining > 0)
            {
                break;   /* 被中止标志截断 */
            }
            /* 分块结尾 CRLF */
            r = http_read_line(line, sizeof(line), timeout_ms);
            if (r < 0)
            {
                esp01s_tcp_close();
                return r;
            }
        }
    }
    else if (resp->content_len > 0)
    {
        remaining = resp->content_len;
        while (remaining > 0)
        {
            if (resp->abort_flag != NULL && *resp->abort_flag != 0)
            {
                break;
            }
            want = (remaining > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;
            r = http_read_body(chunk, want, timeout_ms, on_data, arg, resp);
            if (r <= 0)
            {
                esp01s_tcp_close();
                return (r < 0) ? r : ESP_ERR;
            }
            remaining -= (uint32_t)r;
        }
    }
    else if (keep_body)
    {
        /* 无 Content-Length 亦非 chunked：读至连接关闭（Connection: close） */
        for (;;)
        {
            if (resp->abort_flag != NULL && *resp->abort_flag != 0)
            {
                break;
            }
            r = http_read_body(chunk, sizeof(chunk), timeout_ms, on_data, arg, resp);
            if (r == ESP_CLOSED)
            {
                break;
            }
            if (r == ESP_TIMEOUT)
            {
                esp01s_tcp_close();
                return ESP_TIMEOUT;
            }
            if (r < 0)
            {
                esp01s_tcp_close();
                return r;
            }
        }
    }

    esp01s_tcp_close();
    return ESP_OK;
}

/*
 *  HTTP 请求失败瞬时重试包装：
 *   - 下载场景（on_data != NULL）不重试：重试会令回调重复写同一段 Flash
 *     （请求层不感知 offset），虽然数据幂等，但失败一半再重写有边界风险；
 *   - 普通请求（check/version/status）最多重试 3 次：每次失败先 CIPCLOSE
 *     清残留 TCP 状态并等待 200ms，规避 WiFi 掉线窗口内的偶发失败——
 *     这正是"每次 HTTP 都新建连接、撞上 6.5s 掉线窗口"场景的兜底。
 */
int http_client_request(const char *host, uint16_t port,
                        const char *method, const char *path,
                        const char *extra_hdr, const char *body,
                        http_resp_t *resp,
                        void (*on_data)(const uint8_t *data, uint32_t len, void *arg),
                        void *arg, uint32_t timeout_ms)
{
    int attempt;
    int r;
    int max_attempt = (on_data != NULL) ? 1 : 3;

    for (attempt = 0; attempt < max_attempt; attempt++)
    {
        r = http_client_request_once(host, port, method, path, extra_hdr, body,
                                     resp, on_data, arg, timeout_ms);
        if (r == ESP_OK)
        {
            return ESP_OK;
        }
        if (attempt + 1 < max_attempt)
        {
            esp01s_tcp_close();   /* 清残留 TCP 状态，避免模块 AT 引擎忙 */
            HAL_Delay(200);       /* 给模块/网络短暂恢复时间 */
        }
    }
    return r;
}
