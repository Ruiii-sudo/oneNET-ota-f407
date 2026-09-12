#include "user_task.h"
#include "FreeRTOS.h"
#include "task.h"

#include "LCD.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "touch.h"
#include "XPT2046.h"
#include "ota.h"
#include "ota_ui.h"
#include "w25q16.h"


/*开始任务*/
#define START_TASK_STACK_SIZE 128
#define START_TASK_PRIORITY 2
TaskHandle_t start_task_handle;
void start_task(void *pvParameters);  //start_task任务函数

/*LVGL图像处理任务*/
#define LVGL_Task_STACK_SIZE 1024
#define LVGL_Task_PRIORITY 3
TaskHandle_t LVGL_Task_handle;
void LVGL_Task(void *pvParameters);  //LVGL_Task任务函数

/*触摸扫描任务*/
#define XPT2046_ScanTask_STACK_SIZE 128
#define XPT2046_ScanTask_PRIORITY 4
TaskHandle_t XPT2046_ScanTask_handle;
void XPT2046_ScanTask(void *pvParameters);  //XPT2046_ScanTask任务函数

//OTA任务配置
#define OTA_TASK_STACK_SIZE 1536
#define OTA_TASK_PRIORITY 2
TaskHandle_t OTA_Task_handle;


/**
 * @brief  启动FreeRTOS
 * @retval None
 */
void freertos_start(void)
{
    //1.创建一个开始任务
    xTaskCreate((TaskFunction_t) start_task,                      //任务函数的地址
                (char *) "Start Task",                            //任务名称
                (configSTACK_DEPTH_TYPE) START_TASK_STACK_SIZE,   //任务堆栈大小，单位为字
                (void *)  NULL,                                   //传给任务函数的参数
                (UBaseType_t) START_TASK_PRIORITY,                //任务优先级
                (TaskHandle_t *) &start_task_handle);             //任务句柄的地址

    //2.启动调度器
    vTaskStartScheduler();
}

/**
 * @brief  开始任务：创建各业务任务后自删除
 * @param  pvParameters: 任务参数
 * @retval None
 */
void start_task(void *pvParameters)
{
    taskENTER_CRITICAL(); //进入临界区:临界区内的代码不会被中断

    //创建LVGL图像处理任务
    xTaskCreate((TaskFunction_t) LVGL_Task,                    
                (char *) "LVGL_Task",                          
                (configSTACK_DEPTH_TYPE) LVGL_Task_STACK_SIZE,  
                (void *)  NULL,                             
                (UBaseType_t) LVGL_Task_PRIORITY,               
                (TaskHandle_t *) &LVGL_Task_handle);      

    //创建触摸扫描任务
    xTaskCreate((TaskFunction_t) XPT2046_ScanTask,                    
                (char *) "XPT2046_ScanTask",                          
                (configSTACK_DEPTH_TYPE) XPT2046_ScanTask_STACK_SIZE,  
                (void *)  NULL,                             
                (UBaseType_t) XPT2046_ScanTask_PRIORITY,               
                (TaskHandle_t *) &XPT2046_ScanTask_handle);    

    //创建 OTA 升级任务（优先级 2；网络阻塞期间通过钩子喂狗）
    xTaskCreate((TaskFunction_t)OTA_Task,
                (char *)"OTA_Task",
                (configSTACK_DEPTH_TYPE)OTA_TASK_STACK_SIZE,
                (void *)  NULL, 
                (UBaseType_t)OTA_TASK_PRIORITY,
                (TaskHandle_t *) &OTA_Task_handle); 

    //删除开始任务(开始任务只需要执行一次，任务结束后就可以删除自己)
    vTaskDelete(NULL);

    taskEXIT_CRITICAL(); //退出临界区
}


void LVGL_Task(void *pvParameters)
{
    // LVGL初始化，注意按顺序进行
    USART1_Printf("STEP1: LVGL core init\r\n");
    lv_init();
    USART1_Printf("STEP2: LVGL display init\r\n");
    lv_port_disp_init(); // 显示初始化
    USART1_Printf("STEP3: LVGL indev init\r\n");
    lv_port_indev_init(); // 输入设备初始化（触摸）
             
    // 创建 LVGL demo 主界面
    ota_ui_init();
  	
    USART1_Printf("System init OK, OTA enabled\r\n");
	
    for (;;)
    {
        ota_feed_watchdog();   // 喂独立看门狗（Bootloader 启动，约8s超时）
        lv_task_handler();     // LVGL任务处理
        ota_ui_refresh();      // 刷新 OTA 状态面板
        vTaskDelay(5); 
    }
}


void XPT2046_ScanTask(void *pvParameters)
{
    TP_Init();  // 触摸硬件初始化
    for (;;)
    {
        tp_dev.scan(); // 轮询扫描触摸点（XPT2046无中断，使用轮询）
        vTaskDelay(10); 
    }
}
