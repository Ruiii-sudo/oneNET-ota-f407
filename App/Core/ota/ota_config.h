/**
 ******************************************************************************
 * @file    ota_config.h
 * @brief   OTA 运行配置 —— OneNET/CMIoT 平台内置远程升级（OTA 2.0 / fuse-ota）
 *
 * 适配说明：
 *  - MQTT：订阅 $sys/{pid}/{dev}/ota/inform 接收平台升级任务通知，并回 inform_reply
 *  - HTTP：调用平台南向 OTA 接口（iot-api.heclouds.com / fuse-ota）：
 *      POST /fuse-ota/{pid}/{dev}/version          上报当前版本
 *      GET  /fuse-ota/{pid}/{dev}/check            检测升级任务
 *      GET  /fuse-ota/{pid}/{dev}/{tid}/download   下载升级包（支持 Range 断点续传）
 *      POST /fuse-ota/{pid}/{dev}/{tid}/status     上报下载进度/升级状态
 *  - 校验：升级包以平台下发的 MD5 校验；下载完成后计算 SHA-256 存入参数区，
 *    供 BootLoader 启动前二次校验（沿用原 A/B 双分区 + 回滚机制）。
 ******************************************************************************
 */
#ifndef __OTA_CONFIG_H
#define __OTA_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "partition.h"

#define OTA_APP_BASE   OTA_APP_A_ADDR

/* ---- WiFi ---- */
#define OTA_WIFI_SSID       "CMCC-6DEC"
#define OTA_WIFI_PASS       "efb5Kd5d"

/*
 * ===== OneNET（MQTT 接入）=====
 * 接入参数对应关系（OneNET 官方 MQTT 三要素）：
 *   clientId = 设备名称
 *   username = 产品ID
 *   password = token（设备密钥 + 过期时间 计算出的完整字符串，见下方注释）
 *
 * 服务器域名：
 *   mqtts.heclouds.com        :1883（非加密；实测 IPv4=218.201.45.2）
 *   studio-mqtt.heclouds.com  :1883（官方文档 MQTT 域名，IPv4=218.201.45.7）
 *   studio-mqtts.heclouds.com :8883（TLS，本工程 ESP01S 不启用 TLS，勿用）
 *
 */
#define OTA_MQTT_HOST       "studio-mqtt.heclouds.com"
#define OTA_MQTT_PORT       1883
#define OTA_MQTT_USER       "VuBpEu593g"              /* 产品ID */
#define OTA_MQTT_PASS       "version=2018-10-31&res=products%2FVuBpEu593g%2Fdevices%2Fmytest&et=1805693871&method=md5&sign=evGV8xLIEFRxWpfwpm9S8g%3D%3D"
#define OTA_MQTT_KEEPALIVE  60                       /* 秒 */

/*
 * NOTE（token 有效期）：
 * 本 token 已用设备密钥独立重算核验通过（sign 一致），为真实有效值：
 *   - 产品ID VuBpEu593g / 设备名 mytest
 *   - et=1805693871，即 2027-03-22 13:37（北京时间）到期
 * 到期前需重新生成，算法：
 *   StringForSignature = "1805693871\nmd5\nproducts/VuBpEu593g/devices/mytest\n2018-10-31"
 *   sign = base64(hmac_md5(base64decode(设备密钥), StringForSignature))
 * 重新生成后替换 OTA_MQTT_PASS 即可，设备侧无需改逻辑。
 * OTA_HTTP_AUTH 与该 token 相同（OneNET 南向 OTA 接口使用“设备鉴权信息”）。
 */

/* ---- OneNET 设备（设备名称） ---- */
#define OTA_DEVICE_ID       "mytest"

/*
 * ===== OneNET 平台内置 OTA（远程升级）通信主题 =====
 * 主题由 OTA_MQTT_USER / OTA_DEVICE_ID 拼接，换产品/设备时只需改上面两处。
 * 对应平台文档《物模型数据交互 - 订阅OTA远程升级topic》：
 *   订阅: $sys/{pid}/{device-name}/ota/inform         平台下发系统OTA升级通知
 *   发布: $sys/{pid}/{device-name}/ota/inform_reply   设备应答（需在 20s 内回复）
 */
#define OTA_TOPIC_INFORM        "$sys/" OTA_MQTT_USER "/" OTA_DEVICE_ID "/ota/inform"
#define OTA_TOPIC_INFORM_REPLY  "$sys/" OTA_MQTT_USER "/" OTA_DEVICE_ID "/ota/inform_reply"

/*
 * ===== OneNET 南向 OTA API（fuse-ota，平台“增值服务 -> OTA升级”）=====
 * 域名已实测支持 80 端口明文 HTTP（ESP01S 不支持 TLS）。
 *   POST /fuse-ota/{pro_id}/{dev_name}/version          上报版本
 *   GET  /fuse-ota/{pro_id}/{dev_name}/check            检测任务
 *   GET  /fuse-ota/{pro_id}/{dev_name}/{tid}/download   下载升级包（支持 Range）
 *   POST /fuse-ota/{pro_id}/{dev_name}/{tid}/status     上报进度/状态
 * 若平台后续仅开放 HTTPS，需更换支持 TLS 的联网模组并将端口改为 443。
 */
#define OTA_API_HOST        "iot-api.heclouds.com"
#define OTA_API_PORT        80
/* HTTP Authorization 头：与 MQTT 密码相同（设备鉴权 token）。
 * 若鉴权失败，可改为产品级 token（res=products/{产品ID}，算法相同）。 */
#define OTA_HTTP_AUTH       OTA_MQTT_PASS

/* OTA 任务类型：1=FOTA（模组升级） 2=SOTA（应用升级）——本工程升级 MCU 应用，填 2 */
#define OTA_TASK_TYPE       2

/* 模组版本号（f_version，上报 /version 时使用；ESP01S 无统一版本时填 "1.0.0"） */
#define OTA_FW_VERSION_STR  "1.0.0"

/*
 * ===== 固件版本 =====
 * OTA_APP_VERSION_NUM  用于防降级比较（内部编码，勿改格式）
 * OTA_APP_VERSION_STR  上报平台的版本字符串，必须与平台上"升级包"的版本号
 *                      格式一致（如平台升级包版本填 1.0.1，这里就填 "1.0.1"）。
 * 发布新固件时两处同步修改。
 */
#define OTA_APP_VERSION_NUM   OTA_VERSION(1, 0, 0)
#define OTA_APP_VERSION_STR   "1.0.0"

/* ---- 下载参数 ---- */
#define OTA_DOWNLOAD_BLOCK      4096U   /* 下载块 4KB */
#define OTA_CHECKPOINT_INTERVAL 16384U  /* 断点检查点：每 16KB 写一次参数区 */

/* 下载进度上报平台：每跨过多少百分比上报一次（1-100；越密上报越频繁） */
#define OTA_PROGRESS_STEP      100U

/*
 * 无升级通知时主动轮询检测任务的周期（毫秒）。
 * 0 = 仅靠平台 ota/inform 通知触发（最省流量，但设备离线期间创建的任务
 *     不会在下次上线时自动补检测，需要手动点 CHECK 按钮触发）。
 * 推荐 600000（10 分钟）：上线先检测一次，之后每周期检测一次。
 */
#define OTA_CHECK_PERIOD_MS    600000U

/* ---- 启动成功窗口：App 启动后该时间内保持运行即视为成功 ---- */
#define OTA_STARTUP_CONFIRM_MS  30000U

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CONFIG_H */
