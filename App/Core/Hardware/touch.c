#include "touch.h"

volatile _m_tp_dev tp_dev =
	{
		TP_Init,
		NULL,
		0,
		0,
		0,
};

/**
 * @brief 初始化触摸屏（XPT2046 电阻触摸）
 * @return 初始化结果：0=成功
 */
uint8_t TP_Init(void)
{
	XPT2046_Init();
	tp_dev.init = XPT2046_Init;  /* 初始化函数指向XPT2046初始化 */
	tp_dev.scan = XPT2046_Scan;  /* 扫描函数指向XPT2046扫描 */
	return 0;
}
