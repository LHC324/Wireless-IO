/**
 * @brief	shell:STM32F743IIT6
 * @author	LHC
 * @date	2021/10/18
 */

#include "shell.h"
#include "usart.h"
#include "shell_port.h"

/*定义shell目标端口*/
#define SHELL_TARGET_UART huart1

/*定义shell调试接口*/
#define Printf_Dbug(info) shellPrint(&shell, info);

#if defined(USING_RTTHREAD)
#include "rtthread.h"
extern rt_mutex_t shellMutexHandle;
#else
#include "cmsis_os.h"
extern osMutexId shellMutexHandle;
#endif
/* 定义shell对象*/
Shell shell;
char shell_buffer[SHELL_BUFFER_SIZE];


/**
 * @brief shell读取数据函数原型
 *
 * @param data shell读取的字符
 * @param len 请求读取的字符数量
 *
 * @return unsigned short 实际读取到的字符数量
 */
unsigned short User_Shell_Read(char *data, unsigned short len)
{
	if(HAL_UART_Receive(&SHELL_TARGET_UART, (uint8_t *)data, len, 0x01) != HAL_OK)
	{	/*串口接收数据失败*/
		len = 0;
	}

	return len;
}

/**
 * @brief shell写数据函数原型
 *
 * @param data 需写的字符数据
 * @param len 需要写入的字符数
 *
 * @return unsigned short 实际写入的字符数量
 */
unsigned short User_Shell_Write(char *data, unsigned short len)
{
	if(HAL_UART_Transmit(&SHELL_TARGET_UART, (uint8_t *)data, len, 0xFFFF) != HAL_OK)
	{	/*串口发送数据失败*/
		len = 0;
	}

	return len;
}

/**
 * @brief 用户shell上锁
 * 
 * @param shell shell
 * 
 * @return int 0
 */
int userShellLock(Shell *shell)
{	
#if defined(USING_RTTHREAD)
	rt_mutex_take(shellMutexHandle, RT_WAITING_FOREVER);
#else
	osRecursiveMutexWait (shellMutexHandle, portMAX_DELAY);
#endif
return 0;
}

/**
 * @brief 用户shell解锁
 * 
 * @param shell shell
 * 
 * @return int 0
 */
int userShellUnlock(Shell *shell)
{
#if defined(USING_RTTHREAD)
	/* 释 放 互 斥 锁 */
	rt_mutex_release(shellMutexHandle);
#else
	osRecursiveMutexRelease(shellMutexHandle);
#endif
    return 0;
}


/**
 * @brief shell初始化
 *
 * @param None
 *
 * @return None
 */
void User_Shell_Init(void)
{
	shell.write = User_Shell_Write;
	shell.read = User_Shell_Read;
	shell.lock = userShellLock;
    shell.unlock = userShellUnlock;

	shellInit(&shell, shell_buffer, SHELL_BUFFER_SIZE);
}
