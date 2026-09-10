/**
 ******************************************************************************
 * @file    esp01s.h
 * @brief   ESP-01S WiFi 模块驱动（USART3，AT 指令，TCP 单连接模式）
 *
 * 接线：
 *   PB10 -> USART3_TX -> ESP01S RX
 *   PB11 <- USART3_RX <- ESP01S TX
 *
 * 工作模式：CIPMUX=0（单连接）、CIPMODE=0（非透传）
 * 接收侧自动剥离 "+IPD,<len>:" 帧头，向上层提供干净的字节流。
 ******************************************************************************
 */
#ifndef __ESP01S_H
#define __ESP01S_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* USART3 句柄（定义于 esp01s.c，供 stm32f4xx_it.c 的中断处理使用） */
extern UART_HandleTypeDef huart3;

/* 返回码 */
#define ESP_OK        0
#define ESP_ERR      -1
#define ESP_TIMEOUT  -2
#define ESP_CLOSED   -3

/* 串口接收环形缓冲区大小
   [FIX-8] 1024 -> 2048：下载数据时 OTA 任务被 LVGL 抢占，若中断喂入
   速度超过消费速度会导致丢字节；115200 波特率约 11.5KB/s，
   2048 字节可容忍约 178ms 的消费停顿 */
#define ESP_RX_RING_SIZE   2048U
/* 数据缓冲区（剥离 IPD 头后的字节流） */
#define ESP_DATA_BUF_SIZE  4096U

/* 初始化：USART3 + AT 测试（ATE0 / CIPMUX=0 / CIPMODE=0） */
int  esp01s_init(void);

/* 连接 WiFi：AT+CWJAP，成功返回 ESP_OK */
int  esp01s_join_ap(const char *ssid, const char *pass, uint32_t timeout_ms);

/*
 * 查询 WiFi 连接状态：AT+CWJAP?，应答 +CWJAP:3 视为已连接并获取 IP。
 * 返回 ESP_OK=已连接；ESP_ERR=未连接/ERROR；ESP_TIMEOUT=超时。
 * [FIX-20] 供 OTA 任务在 HTTP 失败时区分"WiFi 掉了"与"纯网络失败"：
 *   WiFi 掉了应立即重连（回 CONNECTING），而不是盲目退避 3s~24s。
 */
int  esp01s_wifi_status(uint32_t timeout_ms);

/* 建立 TCP 连接（单连接模式）：AT+CIPSTART */
int  esp01s_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/* 关闭 TCP 连接：AT+CIPCLOSE */
void esp01s_tcp_close(void);

/* 发送 len 字节（AT+CIPSEND 流程，自动处理 ">" 提示与 SEND OK） */
int  esp01s_send(const uint8_t *data, uint32_t len, uint32_t timeout_ms);

/*
 * 读取最多 len 字节数据（自动剥离 +IPD 帧头）。
 * 返回实际读取字节数；超时返回 ESP_TIMEOUT；连接被关闭返回 ESP_CLOSED
 */
int  esp01s_read(uint8_t *buf, uint32_t len, uint32_t timeout_ms);

/* 供 USART3 中断处理函数调用：放入一个字节 */
void esp01s_feed_byte(uint8_t byte);

/* 丢弃接收缓冲中的全部残留字节（发命令前调用，防止误匹配） */
void esp01s_flush_rx(void);

/*
 * 阻塞等待钩子（弱符号）：长阻塞期间被周期性调用。
 * OTA 任务可提供强定义实现喂狗，防止网络等待时 IWDG 复位。
 */
void esp01s_wait_hook(void);

/* 底层：发送原始 AT 行并等待期望串（供扩展命令使用） */
int  esp01s_at_cmd(const char *cmd, const char *expect, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __ESP01S_H */
