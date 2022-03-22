/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2022 STMicroelectronics.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed by ST under Ultimate Liberty license
 * SLA0044, the "License"; You may not use this file except in compliance with
 * the License. You may obtain a copy of the License at:
 *                             www.st.com/SLA0044
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "shell.h"
#include "shell_port.h"
#include "mdrtuslave.h"
#include "at_usr.h"
#include "io_signal.h"
#include "L101.h"
#include "io_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
bool g_At = false;
SHELL_EXPORT_VAR(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_VAR_INT), g_at, &g_At, at_cmd);
extern void Free_Mode(Shell *shell, char *pData);
extern bool Check_Mode(ModbusRTUSlaveHandler handler);
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
osThreadId shellHandle;
osThreadId atHandle;
osThreadId mdbusHandle;
osThreadId read_ioHandle;
osTimerId Timer1Handle;
osMutexId shellMutexHandle;
osSemaphoreId ReciveHandle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void Shell_Task(void const * argument);
void At_Task(void const * argument);
void Mdbus_Task(void const * argument);
void Read_Io_Task(void const * argument);
void Timer_Callback(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* GetTimerTaskMemory prototype (linked to static allocation support) */
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize );

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);

/* USER CODE BEGIN 4 */
__weak void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
  /* Run time stack overflow checking is performed if
  configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
  called if a stack overflow is detected. */
  shellPrint(&shell, "%s is stack overflow!\r\n", pcTaskName);
}
/* USER CODE END 4 */

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize)
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/* USER CODE BEGIN GET_TIMER_TASK_MEMORY */
static StaticTask_t xTimerTaskTCBBuffer;
static StackType_t xTimerStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize)
{
  *ppxTimerTaskTCBBuffer = &xTimerTaskTCBBuffer;
  *ppxTimerTaskStackBuffer = &xTimerStack[0];
  *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
  /* place for user code */
}
/* USER CODE END GET_TIMER_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */
  /* Create the mutex(es) */
  /* definition and creation of shellMutex */
  osMutexDef(shellMutex);
  shellMutexHandle = osMutexCreate(osMutex(shellMutex));

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* definition and creation of Recive */
  osSemaphoreDef(Recive);
  ReciveHandle = osSemaphoreCreate(osSemaphore(Recive), 1);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* definition and creation of Timer1 */
  osTimerDef(Timer1, Timer_Callback);
  Timer1Handle = osTimerCreate(osTimer(Timer1), osTimerPeriodic, NULL);

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of shell */
  osThreadDef(shell, Shell_Task, osPriorityBelowNormal, 0, 256);
  shellHandle = osThreadCreate(osThread(shell), (void*) &shell);

  /* definition and creation of at */
  osThreadDef(at, At_Task, osPriorityLow, 0, 128);
  atHandle = osThreadCreate(osThread(at), (void*) &shell);

  /* definition and creation of mdbus */
  osThreadDef(mdbus, Mdbus_Task, osPriorityNormal, 0, 256);
  mdbusHandle = osThreadCreate(osThread(mdbus), NULL);

  /* definition and creation of read_io */
  osThreadDef(read_io, Read_Io_Task, osPriorityAboveNormal, 0, 256);
  read_ioHandle = osThreadCreate(osThread(read_io), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  osTimerStart(Timer1Handle, MDTASK_SENDTIMES);
  /*Suspend shell task*/
#if defined(USING_L101)
  osThreadSuspend(shellHandle);
#endif
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_Shell_Task */
/**
 * @brief  Function implementing the shell thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_Shell_Task */
void Shell_Task(void const * argument)
{
  /* USER CODE BEGIN Shell_Task */
  /* Infinite loop */
  for (;;)
  {
#if defined(USING_DEBUG)
    // shellPrint(&shell, "shell_Task is running !\r\n");
    // osDelay(1000);
#endif
    shellTask((void *)argument);
  }
  /* USER CODE END Shell_Task */
}

/* USER CODE BEGIN Header_At_Task */
/**
 * @brief Function implementing the at thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_At_Task */
void At_Task(void const * argument)
{
  /* USER CODE BEGIN At_Task */
  char recv_data = '\0';
  // static uint16_t counter = 0;
  /* Infinite loop */
  for (;;)
  {
#if defined(USING_DEBUG)
    // uint8_t data = 0;
    // extern L101_HandleTypeDef L101_Map[EXTERN_DIGITAL_MAX];
    // L101_Map[0].Check.State = L_OK;
    // L101_Map[1].Check.State = L_OK;
    // L101_Map[7].Check.State = L_OK;
    // HAL_SUART_Receive(&S_Uart1, &data, 1, 0x10);
    // shellPrint(&shell, "flag = %d,Recv_Data = 0x%02x.\r\n", S_Uart1.Rx.Finsh_Flag, data);
    // S_Uart1.Rx.Finsh_Flag = false;
    // osDelay(1000);

    if (g_At)
    {
      /*Suspend mdbushandle task*/
      osThreadSuspendAll();
      osTimerStop(Timer1Handle);
      g_At = false;
      Free_Mode((Shell *)argument, &recv_data);
      /*Restore mdbushandle task*/
      osThreadResumeAll();
      osTimerStart(Timer1Handle, MDTASK_SENDTIMES);
    }
#endif
    // at_process(&at);
    osDelay(5);
  }
  /* USER CODE END At_Task */
}

/* USER CODE BEGIN Header_Mdbus_Task */
/**
 * @brief Function implementing the mdbus thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_Mdbus_Task */
void Mdbus_Task(void const * argument)
{
  /* USER CODE BEGIN Mdbus_Task */
  /* Infinite loop */
  for (;;)
  {
    /*https://www.cnblogs.com/w-smile/p/11333950.html*/
    if (osOK == osSemaphoreWait(ReciveHandle, osWaitForever))
    {
#if defined(USING_L101)
      Check_Mode(Master_Object) ? mdRTU_Handler(Master_Object) : Shell_Mode();
#else
      mdRTU_Handler(Master_Object);
#endif
#if defined(USING_DEBUG)
      shellWriteEndLine(&shell, "Received a data!\r\n", 19U);
      // shellPrint(&shell, "Received a data!\r\n");
      // shellPrint(&shell, "Mdbus_Task is running !\r\n");
      // shellPrint(&shell, "%s\r\n", Master_Object->receiveBuffer->buf);
#endif
    }

#if defined(USING_DEBUG)
//     shellPrint(&shell, "Master_Object = 0x%p, ptick = 0x%p\r\n", Master_Object,
//                Master_Object->portRTUTimerTick);
//     osDelay(1000);
#endif

    // osDelay(10);
  }
  /* USER CODE END Mdbus_Task */
}

/* USER CODE BEGIN Header_Read_Io_Task */
/**
 * @brief Function implementing the read_io thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_Read_Io_Task */
void Read_Io_Task(void const * argument)
{
  /* USER CODE BEGIN Read_Io_Task */
  /* Infinite loop */
  for (;;)
  {
    Io_Digital_Handle();
    Io_Analog_Handle();
    osDelay(50);
  }
  /* USER CODE END Read_Io_Task */
}

/* Timer_Callback function */
void Timer_Callback(void const * argument)
{
  /* USER CODE BEGIN Timer_Callback */
  Master_Poll();
  /* USER CODE END Timer_Callback */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
