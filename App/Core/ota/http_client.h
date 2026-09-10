/**
 ******************************************************************************
 * @file    http_client.h
 * @brief   极简 HTTP/1.1 客户端（基于 ESP01S TCP）
 *
 * 能力：
 *  - GET / POST
 *  - 自定义请求头（Authorization / Range 等）
 *  - POST 请求体
 *  - 响应元数据解析（状态码 / Content-Length / Content-Range / Ota-Errno）
 *  - 大文件流式回调（写 Flash）或小响应体捕获（JSON）
 *  - 支持 Transfer-Encoding: chunked 与"读至连接关闭"两种无 Content-Length 场景
 ******************************************************************************
 */
#ifndef __HTTP_CLIENT_H
#define __HTTP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* HTTP 响应状态码 */
#define HTTP_STATUS_OK       200
#define HTTP_STATUS_PARTIAL  206

/* 响应解析结果 */
typedef struct {
    int      status_code;   /* 200 / 206 / 其他 */
    uint32_t content_len;   /* Content-Length */
    uint32_t total_size;    /* Content-Range 总大小（206 时有效） */
    uint32_t start_offset;  /* Content-Range 起始偏移 */
    int      ota_errno;     /* OneNET 下载接口 Ota-Errno 头；缺省为 -1 */
    /* 小响应体捕获（on_data 为 NULL 且 body 非空时使用） */
    uint8_t *body;          /* 输出缓冲 */
    uint32_t body_cap;      /* 缓冲容量 */
    uint32_t body_len;      /* 实际接收字节数 */
    /* 中止标志：读取正文期间每读一块检查一次，非 0 则截断返回 ESP_OK，
     * 用于"下载一段 -> 上报进度 -> 续传"的轮转（ESP01S 单连接限制）。
     * 可传 NULL。 */
    volatile int *abort_flag;
} http_resp_t;

/*
 * 通用 HTTP 请求。
 *
 * @param host        主机名/IP
 * @param port        端口
 * @param method      "GET" / "POST"
 * @param path        路径（以 '/' 开头，可含 query）
 * @param extra_hdr   附加请求头（如 "Authorization: xxx\r\n"、"Range: bytes=0-\r\n"），
 *                    可传 NULL；末尾无需再加 \r\n
 * @param body        POST 请求体（GET 传 NULL）
 * @param resp        输出响应元数据；其中 body/body_cap 需调用方预置
 * @param on_data     流式数据回调（每收到一段调用一次，写 Flash 用）；NULL 时
 *                    若 resp->body 非空则捕获到 body
 * @param arg         回调参数
 * @param timeout_ms  单段读超时
 * @retval ESP_OK 成功；其余见 esp01s.h 返回码
 */
int http_client_request(const char *host, uint16_t port,
                        const char *method, const char *path,
                        const char *extra_hdr, const char *body,
                        http_resp_t *resp,
                        void (*on_data)(const uint8_t *data, uint32_t len, void *arg),
                        void *arg, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_CLIENT_H */
