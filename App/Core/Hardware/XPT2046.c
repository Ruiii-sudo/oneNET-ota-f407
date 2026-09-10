#include "XPT2046.h"
#include "touch.h"

/* 调试用全局变量 */
volatile uint16_t g_xpt2046_raw_x = 0;
volatile uint16_t g_xpt2046_raw_y = 0;
volatile uint8_t  g_xpt2046_pressed = 0;

/**
 * @brief XPT2046 电阻触摸屏驱动（软件SPI）
 * 适用于 MSP2807 等带 XPT2046 触摸控制器的 2.8寸 ILI9341 模块
 * 改进版：使用中值滤波 + 范围检测，更稳定可靠
 */

/* 软件SPI读写一个字节（MSB first，SPI Mode0） */
static uint8_t XPT2046_SPI_ReadWrite(uint8_t tx_data)
{
    uint8_t rx_data = 0;
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        XPT2046_CLK_LOW();
        if (tx_data & 0x80)
            XPT2046_DIN_HIGH();
        else
            XPT2046_DIN_LOW();
        tx_data <<= 1;
        XPT2046_CLK_HIGH();
        rx_data <<= 1;
        if (XPT2046_DO_READ() == GPIO_PIN_SET)
            rx_data |= 0x01;
    }
    XPT2046_CLK_LOW();
    return rx_data;
}

/**
 * @brief 读取XPT2046的AD转换值
 * @param cmd 命令字（XPT2046_CMD_X / XPT2046_CMD_Y）
 * @return 12位AD值（0~4095）
 */
uint16_t XPT2046_Read_AD(uint8_t cmd)
{
    uint8_t high, low;
    uint16_t value;

    XPT2046_CS_LOW();
    XPT2046_SPI_ReadWrite(cmd);      /* 发送命令 */
    high = XPT2046_SPI_ReadWrite(0); /* 读取高字节 */
    low  = XPT2046_SPI_ReadWrite(0); /* 读取低字节 */
    XPT2046_CS_HIGH();

    /* 12位结果：高字节低7位 + 低字节高5位，右移3位对齐 */
    value = ((uint16_t)high << 8) | low;
    value >>= 3;
    value &= 0x0FFF;
    return value;
}

/**
 * @brief 冒泡排序（用于中值滤波）
 */
static void XPT2046_Sort(uint16_t *arr, uint8_t n)
{
    uint8_t i, j;
    uint16_t temp;
    for (i = 0; i < n - 1; i++)
    {
        for (j = 0; j < n - 1 - i; j++)
        {
            if (arr[j] > arr[j + 1])
            {
                temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

/**
 * @brief 读取一次触摸坐标（带中值滤波）
 * @param x 输出屏幕X坐标
 * @param y 输出屏幕Y坐标
 * @return 1=读取成功，0=未按下或读取失败
 */
uint8_t XPT2046_Read_XY(uint16_t *x, uint16_t *y)
{
    #define SAMPLE_CNT 7
    uint16_t raw_x[SAMPLE_CNT], raw_y[SAMPLE_CNT];
    uint32_t sum_x = 0, sum_y = 0;
    uint16_t avg_x, avg_y;
    int32_t screen_x, screen_y;
    uint8_t i;

    /* 优先使用T_IRQ引脚检测按下（最可靠）
       IRQ低电平=按下，高电平=未按下 */
    if (!XPT2046_IS_PRESSED())
    {
        g_xpt2046_pressed = 0;
        return 0;
    }

    /* 连续采样7次 */
    for (i = 0; i < SAMPLE_CNT; i++)
    {
        raw_x[i] = XPT2046_Read_AD(XPT2046_CMD_X);
        raw_y[i] = XPT2046_Read_AD(XPT2046_CMD_Y);
    }

    /* 排序后去掉最大和最小值，取中间5个的平均（中值滤波） */
    XPT2046_Sort(raw_x, SAMPLE_CNT);
    XPT2046_Sort(raw_y, SAMPLE_CNT);
    for (i = 1; i < SAMPLE_CNT - 1; i++)
    {
        sum_x += raw_x[i];
        sum_y += raw_y[i];
    }
    avg_x = sum_x / (SAMPLE_CNT - 2);
    avg_y = sum_y / (SAMPLE_CNT - 2);

    /* 保存原始AD值到全局变量供调试 */
    g_xpt2046_raw_x = avg_x;
    g_xpt2046_raw_y = avg_y;

    /* IRQ已确认按下，这里只做采样稳定性检查
       正常按下时AD值应在50~4050范围内 */
    if (avg_x < 50 || avg_x > 4050 || avg_y < 50 || avg_y > 4050)
    {
        g_xpt2046_pressed = 0;
        return 0;  /* 采样异常，丢弃 */
    }
    g_xpt2046_pressed = 1;

    /* 检查采样稳定性（最大值-最小值） */
    if ((raw_x[SAMPLE_CNT - 1] - raw_x[0]) > 300 ||
        (raw_y[SAMPLE_CNT - 1] - raw_y[0]) > 300)
    {
        return 0;  /* 采样不稳定，丢弃 */
    }

    /* 原始AD值映射到屏幕坐标
     *
     * 【重要】如果触摸坐标不对，请按以下方式调整：
     * 1. X/Y轴反了：交换 avg_x 和 avg_y
     * 2. 某个轴方向反了：用 MAX - raw 替代 raw - MIN
     * 3. 坐标偏移/范围不对：修改 XPT2046.h 中的 XPT_X_MIN/MAX 和 XPT_Y_MIN/MAX
     *
     * 默认竖屏方向（LCD方向0）的常见映射：
     *   XPT_Y 原始值 → 屏幕 X 坐标
     *   XPT_X 原始值 → 屏幕 Y 坐标
     */
    screen_x = ((int32_t)avg_y - XPT_Y_MIN) * lcddev.width / (XPT_Y_MAX - XPT_Y_MIN);
    screen_y = ((int32_t)XPT_X_MAX - avg_x) * lcddev.height / (XPT_X_MAX - XPT_X_MIN);  /* raw_x向下减小，需反转 */

    /* 边界限制 */
    if (screen_x < 0) screen_x = 0;
    if (screen_x >= lcddev.width) screen_x = lcddev.width - 1;
    if (screen_y < 0) screen_y = 0;
    if (screen_y >= lcddev.height) screen_y = lcddev.height - 1;

    *x = (uint16_t)screen_x;
    *y = (uint16_t)screen_y;
    return 1;
}

/**
 * @brief 检测是否有触摸按下（保留接口，内部调用Read_XY）
 */
uint8_t XPT2046_IsPressed(void)
{
    uint16_t x, y;
    return XPT2046_Read_XY(&x, &y);
}

/**
 * @brief XPT2046 初始化
 */
uint8_t XPT2046_Init(void)
{
    XPT2046_CS_HIGH();
    XPT2046_CLK_LOW();
    XPT2046_DIN_LOW();
    /* 发送一个空命令唤醒芯片 */
    XPT2046_CS_LOW();
    XPT2046_SPI_ReadWrite(0x00);
    XPT2046_CS_HIGH();
    return 0;
}

/**
 * @brief XPT2046 扫描函数（供tp_dev.scan调用）
 */
uint8_t XPT2046_Scan(void)
{
    uint16_t x, y;
    static uint8_t last_pressed = 0;

    if (XPT2046_Read_XY(&x, &y))
    {
        tp_dev.sta = TP_PRES_DOWN | TP_CATH_PRES | 0x01;
        tp_dev.x[0] = x;
        tp_dev.y[0] = y;
        last_pressed = 1;
        return 1;
    }
    else
    {
        if (last_pressed)
        {
            tp_dev.sta &= ~TP_PRES_DOWN;
            last_pressed = 0;
        }
        else
        {
            tp_dev.x[0] = 0xFFFF;
            tp_dev.y[0] = 0xFFFF;
            tp_dev.sta &= 0xE0;
        }
        return 0;
    }
}
