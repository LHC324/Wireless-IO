/**
 ******************************************************************************
 * @brief        AT命令通信管理(OS版本)
 *
 * Copyright (c) 2020, <morro_luo@163.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-01-02     Morro        Initial version.
 * 2021-02-01     Morro        支持URC回调中接收数据.
 * 2021-02-05     Morro        1.修改struct at_obj,去除链表管理机制
 *                             2.删除 at_obj_destroy接口
 * 2021-03-21     Morro        删除at_obj中的at_work_ctx_t域,减少内存使用
 * 2021-04-08     Morro        解决重复释放信号量导致命令出现等待超时的问题
 * 2021-05-15     Morro        优化URC匹配程序
 * 2021-07-20     Morro        增加锁,解决执行at_do_work时urc部分数据丢失问题
 * 2021-11-20     Morro        按单字符处理接收,避免出现在urc事件中读取数据时
 *                             导致后面数据丢失问题
 ******************************************************************************
 */
#include "at.h"
#include "comdef.h"
#include "main.h"
#include "mdrtuslave.h"
#if defined(USING_RTTHREAD)
#include "rtthread.h"
#else
#include "cmsis_os.h"
#endif
#include "shell_port.h"

#if defined(USING_AT)

#define MAX_AT_LOCK_TIME 60 * 1000 // AT命令锁定时间

static void urc_recv_process(at_obj_t *at, const char *buf, unsigned int size);

/**
 * @brief    默认调试接口
 */
static void nop_dbg(const char *fmt, ...) {}

/**
 * @brief    输出字符串
 */
static void put_string(at_obj_t *at, const char *s)
{
    while (*s != '\0')
        at->adap.write(s++, 1);
}

/**
 * @brief    输出字符串(带换行)
 */
static void put_line(at_obj_t *at, const char *s)
{
    put_string(at, s);
    put_string(at, "\r\n");
    at->adap.debug("->\r\n%s\r\n", s);
}

/**
 * @brief    作业读操作
 */
static unsigned int at_work_read(struct at_work_ctx *e, void *buf, unsigned int len)
{
    at_obj_t *at = e->at;
    len = at->adap.read(buf, len);
    urc_recv_process(at, buf, len); //递交到URC进行处理
    return len;
}

/**
 * @brief    作业写操作
 */
static unsigned int at_work_write(struct at_work_ctx *e, const void *buf, unsigned int len)
{
    return e->at->adap.write(buf, len);
}

//打印输出
static void at_work_print(struct at_work_ctx *e, const char *cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    char buf[MAX_AT_CMD_LEN];
    vsnprintf(buf, sizeof(buf), cmd, args);
    put_line(e->at, buf);
    va_end(args);
}

//等待AT命令响应
static at_return wait_resp(at_obj_t *at, at_respond_t *r)
{
    at->ret = AT_RET_TIMEOUT;
    at->resp_timer = at_get_ms();
    at->rcv_cnt = 0; //清空接收缓存
    at->resp = r;
    at_sem_wait(at->completed, r->timeout);
    at->adap.debug("<-\r\n%s\r\n", r->recvbuf);
    at->resp = NULL;
    return at->ret;
}

/**
 * @brief       等待接收到指定串
 * @param[in]   resp    - 期待待接收串(如"OK",">")
 * @param[in]   timeout - 等待超时时间
 */
at_return wait_recv(struct at_work_ctx *e, const char *resp, unsigned int timeout)
{
    char buf[64];
    int cnt = 0, len;
    at_obj_t *at = e->at;
    at_return ret = AT_RET_TIMEOUT;
    unsigned int timer = at_get_ms();
    while (at_get_ms() - timer < timeout)
    {
        len = e->read(e, &buf[cnt], sizeof(buf) - cnt);
        if (len > 0)
        {
            cnt += len;
            buf[cnt] = '\0';
            if (strstr(buf, resp))
            {
                ret = AT_RET_OK;
                break;
            }
            else if (strstr(buf, "ERROR"))
            {
                ret = AT_RET_ERROR;
                break;
            }
        }
        else
            at_delay(1);
    }
    at->adap.debug("%s\r\n", buf);
    return ret;
}

/**
 * @brief       创建AT控制器
 * @param[in]   adap - AT接口适配器
 */
void at_obj_init(at_obj_t *at, const at_adapter_t *adap)
{
    at->adap = *adap;
    at->rcv_cnt = 0;
    at->send_lock = at_sem_new(1);
    at->recv_lock = at_sem_new(1);
    at->completed = at_sem_new(0);
    at->urc_item = NULL;
    if (at->adap.debug == NULL)
        at->adap.debug = nop_dbg;
}

/**
 * @brief       执行命令
 * @param[in]   fmt    - 格式化输出
 * @param[in]   r      - 响应参数,如果填NULL, 默认返回OK表示成功,等待5s
 * @param[in]   args   - 如变参数列表
 */
at_return at_do_cmd(at_obj_t *at, at_respond_t *r, const char *cmd)
{
    at_return ret;
    char defbuf[64];
    at_respond_t default_resp = {"OK", defbuf, sizeof(defbuf), 5000};
    if (r == NULL)
    {
        r = &default_resp; //默认响应
    }
    if (!at_sem_wait(at->send_lock, r->timeout))
    {
        return AT_RET_TIMEOUT;
    }
    at->busy = true;

    while (at->urc_cnt)
    {
        at_delay(10);
    }
    put_line(at, cmd);
    ret = wait_resp(at, r);
    at_sem_post(at->send_lock);
    at->busy = false;
    return ret;
}

/**
 * @brief       执行AT作业
 * @param[in]   at    - AT控制器
 * @param[in]   work  - 作业入口函数(类型 - int (*)(at_work_ctx_t *))
 * @param[in]   params- 作业参数
 * @return      依赖于work的返回值
 */
int at_do_work(at_obj_t *at, at_work work, void *params)
{
    at_work_ctx_t ctx;
    int ret;
    if (!at_sem_wait(at->send_lock, MAX_AT_LOCK_TIME))
    {
        return AT_RET_TIMEOUT;
    }
    if (!at_sem_wait(at->recv_lock, MAX_AT_LOCK_TIME))
        return AT_RET_TIMEOUT;

    at->busy = true;
    //构造at_work_ctx_t
    ctx.params = params;
    ctx.printf = at_work_print;
    ctx.read = at_work_read;
    ctx.write = at_work_write;
    ctx.wait_resp = wait_recv;
    ctx.at = at;

    at->rcv_cnt = 0;
    ret = work(&ctx);
    at_sem_post(at->recv_lock);
    at_sem_post(at->send_lock);
    at->busy = false;
    return ret;
}

/**
 * @brief       分割响应行
 * @param[in]   recvbuf  - 接收缓冲区
 * @param[out]  lines    - 响应行数组
 * @param[in]   separator- 分割符(, \n)
 * @return      行数
 */
int at_split_respond_lines(char *recvbuf, char *lines[], int count, char separator)
{
    char *s = recvbuf;
    size_t i = 0;
    if (s == NULL || lines == NULL)
        return 0;

    lines[i++] = s;
    while (*s && i < count)
    {
        if (*s == ',')
        {
            *s = '\0';
            lines[i++] = s + 1; /*指向下一个子串*/
        }
        s++;
    }
    return i;
}
/**
 * @brief       从URC缓冲区中查询URC项
 * @param[in]   urcline - URC行
 * @return      true - 正常识别并处理, false - 未识别URC
 */
const urc_item_t *find_urc_item(at_obj_t *at, char *urc_buf, unsigned int size)
{
    const urc_item_t *tbl = at->adap.utc_tbl;
    int i;
    if (size < 2)
        return NULL;
    for (i = 0; i < at->adap.urc_tbl_count; i++)
    {
        if (strstr(urc_buf, tbl->prefix))
            return tbl;
        tbl++;
    }
    return NULL;
}

/**
 * @brief       urc 处理总入口
 * @param[in]   urcline - URC行
 */
static void urc_handler_entry(at_obj_t *at, char *urcline, unsigned int size)
{
    at_urc_ctx_t context = {at->adap.read, urcline, at->adap.urc_bufsize, size};
    at->adap.debug("<=\r\n%s\r\n", urcline);
    at->urc_item->handler(&context); /* 递交到上层处理 */
}

/**
 * @brief       urc 接收处理
 * @param[in]   ch  - 接收字符
 * @return      none
 */
static void urc_recv_process(at_obj_t *at, const char *buf, unsigned int size)
{
    register char *urc_buf;
    int ch;
    urc_buf = at->adap.urc_buf;

    //接收超时处理,默认MAX_URC_RECV_TIMEOUT
    if (at->urc_cnt > 0 && at_istimeout(at->urc_timer, MAX_URC_RECV_TIMEOUT))
    {
        urc_buf[at->urc_cnt] = '\0';
        if (at->urc_cnt > 2)
            at->adap.debug("urc recv timeout=>%s\r\n", urc_buf);
        at->urc_cnt = 0;
        at->urc_item = NULL;
    }

    while (size--)
    {
        at->urc_timer = at_get_ms();
        ch = *buf++;
        urc_buf[at->urc_cnt++] = ch;

        if (strchr(SPEC_URC_END_MARKS, ch) || ch == '\0')
        { //结束标记列表
            urc_buf[at->urc_cnt] = '\0';
            if (at->urc_item == NULL)
                at->urc_item = find_urc_item(at, urc_buf, at->urc_cnt);
            if (at->urc_item != NULL)
            {
                if (strchr(at->urc_item->end_mark, ch))
                { //匹配结束标记
                    urc_handler_entry(at, urc_buf, at->urc_cnt);
                    at->urc_cnt = 0;
                    at->urc_item = NULL;
                }
            }
            else if (ch == '\r' || ch == '\n' || ch == '\0')
            {
                if (at->urc_cnt > 2 && !at->busy)
                {
                    at->adap.debug("%s\r\n", urc_buf); //未识别到的URC
                }
                at->urc_cnt = 0;
            }
        }
        else if (at->urc_cnt >= at->adap.urc_bufsize)
        { //溢出处理
            at->urc_cnt = 0;
            at->urc_item = NULL;
        }
    }
}

/**
 * @brief       命令响应通知
 * @return      none
 */
static void resp_notification(at_obj_t *at, at_return ret)
{
    at->ret = ret;
    at->resp = NULL;

    at_sem_post(at->completed);
}

/**
 * @brief       指令响应接收处理
 * @param[in]   buf  - 接收缓冲区
 * @param[in]   size - 缓冲区数据长度
 * @return      none
 */
static void resp_recv_process(at_obj_t *at, const char *buf, unsigned int size)
{
    char *rcv_buf;
    unsigned short rcv_size;
    at_respond_t *resp = at->resp;

    if (resp == NULL) //无命令请求
        return;

    if (size)
    {
        rcv_buf = (char *)resp->recvbuf;
        rcv_size = resp->bufsize;

        if (at->rcv_cnt + size >= rcv_size)
        { //接收溢出
            at->rcv_cnt = 0;
            at->adap.debug("Receive overflow:%s", rcv_buf);
        }
        /*将接收到的数据放至rcv_buf中 ---------------------------------------------*/
        memcpy(rcv_buf + at->rcv_cnt, buf, size);
        at->rcv_cnt += size;
        rcv_buf[at->rcv_cnt] = '\0';

        if (strstr(rcv_buf, resp->matcher))
        { //接收匹配
            resp_notification(at, AT_RET_OK);
            return;
        }
        else if (strstr(rcv_buf, "ERROR"))
        {
            resp_notification(at, AT_RET_ERROR);
            return;
        }
    }

    if (at_istimeout(at->resp_timer, resp->timeout)) //接收超时
        resp_notification(at, AT_RET_TIMEOUT);
    else if (at->suspend) //强制终止
        resp_notification(at, AT_RET_ABORT);
}

/**
 * @brief       AT忙判断
 * @return      true - 有AT指令或者任务正在执行中
 */
bool at_obj_busy(at_obj_t *at)
{
    return !at->busy && at_istimeout(at->urc_timer, 2000);
}

/**
 * @brief       挂起AT作业
 * @return      none
 */
void at_suspend(at_obj_t *at)
{
    at->suspend = 1;
}

/**
 * @brief       恢复AT作业
 * @return      none
 */
void at_resume(at_obj_t *at)
{
    at->suspend = 0;
}

/**
 * @brief       AT处理
 * @return      none
 */
void at_process(at_obj_t *at)
{
    char buf[1];
    unsigned int len;
    if (!at_sem_wait(at->recv_lock, MAX_AT_LOCK_TIME))
        return;
    do
    {
        len = at->adap.read(buf, sizeof(buf));
        urc_recv_process(at, buf, len);
        resp_recv_process(at, buf, len);
    } while (len);

    at_sem_post(at->recv_lock);
}
#else
/*AT等待时间*/
#define AT_WAITTIMES 300U
/*设置/查询模块 AT 命令回显设置*/
#define SECHO "OFF"
/*查询/设置串口参数*/
#define BAUD_PARA "115200,8,1,NONE,NFC"
/*设置查询工作模式*/
#define SWORK_MODE "FP"
/*查询设置休眠模式*/
#define SPOWER_MODE "RUN"
/*空闲时间:3~240 单位秒（默认 20）*/
#define SIMT "20"
/*time： 500,1000,1500,2000,2500,3000,3500,4000ms（默认 2000）*/
#define SWTM "2000"
/*class： 1~10（默认 10）*/
/*速率对应关系（速率为理论峰值，实际速度要较小一些）：
 1: 268bps
 2: 488bps
 3: 537bps
 4: 878bps
 5: 977bps
 6: 1758bps
 7: 3125bps
 8: 6250bps
 9: 10937bps
 10: 21875bps*/
#define SSPD "10"
/*addr： 0~65535（默认 0）65535 为广播地址，同信道同速率的模块都能接收*/
#define SADDR "0"
/*L102-L 工作频段：(398+ch)MHz*/
#define SCH "0"
/**/
#define SFEC "ON"
/*10~20（默认 20db）不推荐使用小功率发送，其电源利用效率不高*/
#define SPWR "20"
/*time： 0~15000ms（默认 500）仅在 LR/LSR 模式下有效，表示进入接收状态所持续的最长时间，当速率等级较慢的时候应适
当的增加该值以保证数据不会被截断。LSR 模式下如果该值设置为 0 则模块发送数据后不开启接收。*/
#define SRTO "500"
/*time： 相邻数据包间隔，范围：100~60000ms*/
#define SSQT "1000"
/*key：16 字节 HEX 字符串*/
#define SKEY "30313233343536373839414243444546"
/*设置/查询快速进入低功耗使能标志,sta： 1 为打开，0 为关闭。*/
#define SPFLAG "0"
/*设置/查询快速进入低功耗数据
Data：123456（默认 123456）。
Style：ascii、hex（默认 ascii）。*/
#define SPDATE "123456,hex"
/*设置/查询发送完成回复标志,sta：1 为打开，0 为关闭。*/
#define SSENDOK "0"

extern UART_HandleTypeDef huart1;

extern osThreadId shellHandle;
extern osThreadId mdbusHandle;
extern osTimerId Timer1Handle;
extern osSemaphoreId ReciveHandle;

/*定义一个标志，在配置模式下，操作系统相关任务不执行*/
// bool g_Modbus_ExeFlag = false;

typedef enum
{
    CONF_MODE = 0x00,
    FREE_MODE,
    UNKOWN_MODE,
    USER_ESC,
    CONF_ERROR,
    CONF_TOMEOUT,
    CONF_SUCCESS,
    INPUT_ERROR,
    CMD_MODE, //
    CMD_SURE,
    SET_ECHO,
    SET_UART,
    WORK_MODE,
    POWER_MODE,
    SET_TIDLE,
    SET_TWAKEUP,
    SPEED_GRADE,
    TARGET_ADDR,
    CHANNEL,
    CHECK_ERROR,
    TRANS_POWER,
    SET_OUTTIME,
    // SET_KEY,
    RESTART,
    SIG_STREN,
    EXIT_CMD,
    RECOVERY,
    SELECT_NID,
    SELECT_VER,
    LOW_PFLAG,
    LOW_PDATE,
    FINISH_FLAG,
    EXIT_CONF,
    NO_CMD
} At_InfoList;

typedef struct
{
    At_InfoList Name;
    char *pSend;
    char *pRecv;
    void (*event)(char *data);
} At_HandleTypeDef __attribute__((aligned(4)));

// typedef struct
// {
//     char *pRecv;
//     uint16_t WaitTimes;
// }AT_COMM_HandleTypeDef __attribute__((aligned(4)));

// static uint16_t g_ATWait_Times = AT_WAITTIMES;

At_HandleTypeDef At_Table[] = {
    {.Name = CMD_MODE, .pSend = "+++", "a", NULL},
    {.Name = CMD_SURE, .pSend = "a", AT_CMD_OK, NULL},
    {.Name = EXIT_CMD, .pSend = "AT+ENTM", NULL, NULL},                                  /*退出命令模式，恢复原工作模式*/
    {.Name = SET_ECHO, .pSend = "AT+E=" SECHO, "AT+E", NULL},                            /*设置/查询模块 AT 命令回显设置*/
    {.Name = RESTART, .pSend = "AT+Z", "LoRa Start!", NULL},                             /*重启模块*/
    {.Name = RECOVERY, .pSend = "AT+CFGTF", "+CFGTF:SAVED", NULL},                       /*复制当前配置参数为用户默认出厂配置*/
    {.Name = SELECT_NID, .pSend = "AT+NID", "+NID:", NULL},                              /*查询模块节点 ID*/
    {.Name = SELECT_VER, .pSend = "AT+VER", "+VER:", NULL},                              /*查询模块固件版本*/
    {.Name = SET_UART, .pSend = "AT+UART=" BAUD_PARA, "+UART:" BAUD_PARA, NULL},         /*设置串口参数*/
    {.Name = WORK_MODE, .pSend = "AT+WMODE=" SWORK_MODE, "+WMODE:" SWORK_MODE, NULL},    /*设置工作模式*/
    {.Name = POWER_MODE, .pSend = "AT+PMODE=" SPOWER_MODE, "+PMODE:" SPOWER_MODE, NULL}, /*设置功耗模式*/
    {.Name = SET_TIDLE, .pSend = "AT+ITM=" SIMT, "+ITM:" SIMT, NULL},                    /*设置空闲时间:LR/LSR模式有效*/
    {.Name = SET_TWAKEUP, .pSend = "AT+WTM=" SWTM, "+WTM:" SWTM, NULL},                  /*设置唤醒间隔：此参数对 RUN、LSR 模式无效*/
    {.Name = SPEED_GRADE, .pSend = "AT+SPD=" SSPD, "+SPD:" SSPD, NULL},                  /*设置速率等级*/
    {.Name = TARGET_ADDR, .pSend = "AT+ADDR=" SADDR, "+ADDR:" SADDR, NULL},              /*设置目的地址*/
    {.Name = CHANNEL, .pSend = "AT+CH=" SCH, "+CH:" SCH, NULL},                          /*设置信道*/
    {.Name = CHECK_ERROR, .pSend = "AT+FEC=" SFEC, "+FEC:" SFEC, NULL},                  /*设置前向纠错*/
    {.Name = TRANS_POWER, .pSend = "AT+PWR=" SPWR, "+PWR:" SPWR, NULL},                  /*设置发射功率*/
    {.Name = SET_OUTTIME, .pSend = "AT+RTO=" SRTO, "+RTO:" SRTO, NULL},                  /*设置接收超时时间*/
    {.Name = SIG_STREN, .pSend = "AT+SQT=" SSQT, "+SQT:" SSQT, NULL},                    /*查询信号强度/设置数据自动发送间隔*/
    // {.Name = SET_KEY, .pSend = "AT+KEY=" SKEY, "+KEY:" SKEY, NULL},                      /*设置数据加密字*/
    {.Name = LOW_PFLAG, .pSend = "AT+PFLAG=" SPFLAG, "+PFLAG:" SPFLAG, NULL},       /*设置/查询快速进入低功耗使能标志*/
    {.Name = LOW_PDATE, .pSend = "AT+PDATE=" SPDATE, "+PDATE:" SPDATE, NULL},       /*设置/查询快速进入低功耗数据*/
    {.Name = FINISH_FLAG, .pSend = "AT+SENDOK=" SSENDOK, "+SENDOK:" SSENDOK, NULL}, /*设置/查询发送完成回复标志*/
};
#define AT_TABLE_SIZE (sizeof(At_Table) / sizeof(At_HandleTypeDef))

static const char *atText[] = {
    [CONF_MODE] = "Note: Enter configuration!\r\n",
    [FREE_MODE] = "Note: Enter free mode!\r\n",
    [UNKOWN_MODE] = "Error: Unknown mode!\r\n",
    [USER_ESC] = "Warning: User cancel!\r\n",
    [CONF_ERROR] = "Error: Configuration failed!\r\n",
    [CONF_TOMEOUT] = "Error: Configuration timeout.\r\n",
    [CONF_SUCCESS] = "Success: Configuration succeeded!\r\n",
    [INPUT_ERROR] = "Error: Input error!\r\n",
    [CMD_MODE] = "Note: Enter transparent mode!\r\n",
    [CMD_SURE] = "Note: Confirm to exit the transparent transmission mode?\r\n",
    [SET_ECHO] = "Note: Set echo?\r\n",
    [SET_UART] = "Note: Set serial port parameters!\r\n",
    [WORK_MODE] = "Note: Please enter the working mode?(0:TRANS/1:FP)\r\n",
    [POWER_MODE] = "Note: Please enter the power consumption mode?(0:RUN/1:LR/2:WU/3:LSR)\r\n",
    [SET_TIDLE] = "Note: Set idle time.\r\n",
    [SET_TWAKEUP] = "Note: Set wake-up interval.\r\n",
    [SPEED_GRADE] = "Note: Please enter the rate level?(1~10)\r\n",
    [TARGET_ADDR] = "Note: Please enter the destination address?(0~65535)\r\n",
    [CHANNEL] = "Note: Please enter the channel?(0~127)\r\n",
    [CHECK_ERROR] = "Note: Enable forward error correction?(1:true/0:false)\r\n",
    [TRANS_POWER] = "Note: Please input the transmission power?(10~20db)\r\n",
    [SET_OUTTIME] = "Note: Please enter the receiving timeout?(LR/LSR mode is valid,0~15000ms)\r\n",
    // [SET_KEY] = "Note: Please enter the data encryption word?(16bit Hex)\r\n",
    [RESTART] = "Note: Device restart!\r\n",
    [SIG_STREN] = "Note: Query signal strength.\r\n",
    [EXIT_CMD] = "Note: Exit command mode!\r\n",
    [RECOVERY] = "Note: Restore default parameters!\r\n",
    [SELECT_NID] = "Note: Query node ID?\r\n",
    [SELECT_VER] = "Note: Query version number?\r\n",
    [LOW_PFLAG] = "Note: Set / query fast access low power enable flag.\r\n",
    [LOW_PDATE] = "Note: Set / query fast access to low-power data.\r\n",
    [FINISH_FLAG] = "Note: Set / query sending completion reply flag.\r\n",
    [EXIT_CONF] = "Note: Please press \"ESC\" to end the configuration!\r\n",
    [NO_CMD] = "Error: Command does not exist!\r\n",
};

/**
 * @brief       等待接收到指定串
 * @param[in]   resp    - 期待待接收串(如"OK",">")
 * @param[in]   timeout - 等待超时时间
 */
At_InfoList Wait_Recv(Shell *const shell, const ReceiveBufferHandle pB, const char *resp, uint16_t timeout)
{
    At_InfoList ret = CONF_TOMEOUT;
    uint16_t timer = HAL_GetTick();

    // ret = CONF_SUCCESS;
#if defined(USING_DEBUG)
    // shellPrint(shell, ">wait:%s\r\n", resp);
#endif
    // while (HAL_GetTick() - timer < timeout)
    // {
    //     osDelay(1);
    // };

    while (!pB->count)
    {
        osDelay(1);
    };
#if defined(USING_DEBUG)
        // shellPrint(shell, "pB->count:%d\r\n", pB->count);
        // shellPrint(shell, "pB:%p\r\n", pB);
#endif
    if (pB->count)
    {
        pB->buf[pB->count] = '\0';
        ret = strstr((const char *)&(pB->buf), resp) ? CONF_SUCCESS : (strstr((const char *)&(pB->buf), AT_CMD_ERROR) ? CONF_ERROR : CONF_TOMEOUT);
#if defined(USING_DEBUG)
        // shellPrint(shell, ">[MCU<-L101]:%s, timer = %d\r\n", pB->buf, timer);
        shellPrint(shell, ">[MCU<-L101]:%s\r\n", pB->buf);
#endif
    }
    mdClearReceiveBuffer(pB);

    return ret;
}

/**
 * @brief  获取目标at指令
 * @param  shell 终端对象
 * @param  data  接收的数据
 * @retval None
 */
At_HandleTypeDef *Get_AtCmd(At_HandleTypeDef *pAt, At_InfoList list, uint16_t size)
{
    for (uint16_t i = 0; i < size; i++)
    {
        if (pAt[i].Name == list)
        {
            return &pAt[i];
        }
    }

    return NULL;
}

/**
 * @brief  自由模式
 * @note   关于多次运行后：接收错误、从中断中进入临界区：https://www.freertos.org/FreeRTOS_Support_Forum_Archive/August_2006/freertos_portENTER_critical_1560359.html
 * @param  shell 终端对象
 * @param  data  接收的数据
 * @retval None
 */
void Free_Mode(Shell *shell, char *pData)
{
#if defined(USING_FREERTOS)
#define SIZE 64U
    char *pBuf = (char *)CUSTOM_MALLOC(sizeof(char) * SIZE);
    if (!pBuf)
    {
        return;
    }
#else
    char pBuf[64U] = {0};
#endif
    ModbusRTUSlaveHandler pH = Master_Object;
    At_HandleTypeDef *pAt = At_Table, *pS = NULL;
    At_InfoList result = CONF_SUCCESS;
    char *pRe = NULL, *pDest = NULL;
    uint16_t len = 0;

    //     while ((*pData) != ESC_CODE)
    //     {
    //         shell->read(pData, 0x01);
    // #if defined(USING_DEBUG)
    //         // shellPrint(shell, "return = 0x%02x\r\n", shell->read(pData, 0x01));
    //         // SHELL_LOCK(shell);
    //         shellPrint(shell, "*pdata = 0x%02x\r\n", *pData);
    //         // SHELL_UNLOCK(shell);
    // #endif
    //     }

#if defined(USING_DEBUG)
    // shellPrint(shell, "pH = 0x%02x\r\n", pH);
#endif
    while (1)
    {
        if (shell->read(pData, 0x01))
        {
            switch (*pData)
            {
            case ENTER_CODE:
            {
                pBuf[len] = '\0';
                len = 0U;
#if defined(USING_DEBUG)
                shellPrint(shell, "\r\nInput:%s\r\n", pBuf);
                // shellWriteEndLine(shell, pBuf, len);
#endif
                /*进入命令模式，并关闭回显*/
                for (At_InfoList at_cmd = CMD_MODE; at_cmd < POWER_MODE; at_cmd++)
                { /*执行完毕后退出指令模式*/
                    at_cmd = (at_cmd == POWER_MODE - 1U) ? RESTART : at_cmd;
                    pS = Get_AtCmd(pAt, at_cmd, AT_TABLE_SIZE);
                    // pRe = ((at_cmd == CMD_MODE) || (at_cmd == RESTART) || (at_cmd == SET_ECHO)) ? pS->pRecv : AT_CMD_OK;
                    // pRe = ((at_cmd == CMD_MODE) || (at_cmd == RESTART)) ? pS->pRecv : AT_CMD_OK;
                    pRe = ((at_cmd == CMD_MODE) || (at_cmd == SET_ECHO)) ? pS->pRecv : AT_CMD_OK;
                    if (pS)
                    {
#if defined(USING_FREERTOS)
                        char *pStr = (char *)CUSTOM_MALLOC(sizeof(char) * strlen(pS->pSend) + strlen(AT_CMD_END_MARK_CRLF));
                        if (pStr)
                        { /*拷贝时带上'\0'*/
                            memcpy(pStr, pS->pSend, strlen(pS->pSend) + 1U);
                            if (at_cmd > CMD_SURE)
                            {
                                strcat(pStr, AT_CMD_END_MARK_CRLF);
                            }
                            pDest = (at_cmd != SET_UART) ? pStr : strcat(pBuf, AT_CMD_END_MARK_CRLF);
#if defined(USING_DEBUG)
                            // SHELL_LOCK(shell);
                            shellPrint(shell, "\r\n%s", atText[at_cmd]);
                            shellPrint(shell, ">[MCU->L101]:%s\r\n", pDest);
                            // SHELL_UNLOCK(shell);
#endif
                            pH->mdRTUSendString(pH, (mdU8 *)pDest, strlen(pDest));
                        }
                        CUSTOM_FREE(pStr);
#else
                        pH->mdRTUSendString(pH, (mdU8 *)pDest, strlen(pDest));
                        /*加上结尾*/
                        if (at_cmd > CMD_SURE)
                        {
                            pH->mdRTUSendString(pH, (mdU8 *)AT_CMD_END_MARK_CRLF, strlen(AT_CMD_END_MARK_CRLF));
                        }
#endif
                    }
                    // SHELL_LOCK(shell);
                    result = Wait_Recv(shell, pH->receiveBuffer, pRe, MAX_URC_RECV_TIMEOUT);
                    // SHELL_UNLOCK(shell);
                    shellWriteString(shell, atText[result]);
                    if ((result != CONF_SUCCESS) || (at_cmd == RESTART))
                    {
                        shellWriteString(shell, atText[EXIT_CONF]);
                        // break;
                    }
                }
            }
            break;
            case BACKSPACE_CODE:
            {
                len = len ? shellDeleteCommandLine(shell, 0x01), --len : 0;
            }
            break;
            case ESC_CODE:
            {
                goto __exit;
            }
            default:
            {
#if defined(USING_DEBUG)
                // shellPrint(shell, "\r\nlen = %d\r\n", len);
#endif
                pBuf[len++] = *pData;
                // shellPrint(shell, "\r\nval = %c\r\n", pBuf[len]);
                len = (len < SIZE) ? len : 0U;
                shell->write(pData, 0x01);
            }
            break;
            }
        }
    }

__exit:
#if defined(USING_FREERTOS)
    CUSTOM_FREE(pBuf);
#endif
}

/**
 * @brief  参数配置模式
 * @param  shell 终端对象
 * @param  data  接收的数据
 * @retval None
 */
static void Config_Mode(Shell *shell, char *pData)
{
    ModbusRTUSlaveHandler pH = Master_Object;
    At_HandleTypeDef *pAt = At_Table, *pS = NULL;
    char *pRe = NULL;
    At_InfoList result = CONF_SUCCESS;
    bool at_mutex = false;

    // mdClearReceiveBuffer(pH->receiveBuffer);
    // HAL_UART_Receive_DMA(&huart1, mdRTU_Recive_Buf(Master_Object), MODBUS_PDU_SIZE_MAX);
    while ((*pData) != ESC_CODE)
    { /*接收到的数据不为0*/
        if (shell->read(pData, 0x01))
        {
#if defined(USING_DEBUG)
            // shellPrint(shell, "*pdata = 0x%02x\r\n", *pData);
#endif
            for (At_InfoList at_cmd = CMD_MODE; (at_cmd < SIG_STREN) && (!at_mutex); at_cmd++)
            {
                pS = Get_AtCmd(pAt, at_cmd, AT_TABLE_SIZE);
#if defined(USING_DEBUG)
                // shellPrint(shell, "at_cmd = %d, %s", at_cmd, atText[at_cmd]);
                // shellPrint(shell, "huart->RxState = %d\r\n", huart1.RxState);
                shellPrint(shell, "\r\n%s", atText[at_cmd]);
                shellPrint(shell, ">[MCU->L101]:%s\r\n", pS->pSend);
#endif
                if (pS->pSend)
                {
#if defined(USING_FREERTOS)
                    char *pStr = (char *)CUSTOM_MALLOC(sizeof(char) * strlen(pS->pSend) + strlen(AT_CMD_END_MARK_CRLF));
                    if (pStr)
                    { /*拷贝时带上'\0'*/
                        memcpy(pStr, pS->pSend, strlen(pS->pSend) + 1U);
                        if (at_cmd > CMD_SURE)
                        {
                            strcat(pStr, AT_CMD_END_MARK_CRLF);
                        }
                        pH->mdRTUSendString(pH, (mdU8 *)pStr, strlen(pStr));
                    }
                    CUSTOM_FREE(pStr);
#else
                    pH->mdRTUSendString(pH, (mdU8 *)pS->pSend, strlen(pS->pSend));
                    /*加上结尾*/
                    if (at_cmd > CMD_SURE)
                    {
                        pH->mdRTUSendString(pH, (mdU8 *)AT_CMD_END_MARK_CRLF, strlen(AT_CMD_END_MARK_CRLF));
                    }
#endif
                }
                else
                {
                    at_mutex = true;
                    shellWriteString(shell, atText[NO_CMD]);
                    break;
                }
                // pRe = ((at_cmd == CMD_MODE) || (at_cmd == RESTART) || (at_cmd == SET_ECHO)) ? pS->pRecv : AT_CMD_OK;
                // pRe = ((at_cmd == CMD_MODE) || (at_cmd == RESTART)) ? pS->pRecv : AT_CMD_OK;
                pRe = ((at_cmd == CMD_MODE) || (at_cmd == SET_ECHO)) ? pS->pRecv : AT_CMD_OK;
                result = Wait_Recv(shell, pH->receiveBuffer, pRe, MAX_URC_RECV_TIMEOUT);
                // result = Wait_Recv(shell, pH->receiveBuffer, pS->pRecv, MAX_URC_RECV_TIMEOUT);
                shellWriteString(shell, atText[result]);
                if ((result != CONF_SUCCESS) || (at_cmd == RESTART))
                {
                    at_mutex = true;
                    shellWriteString(shell, atText[EXIT_CONF]);
                    break;
                }
            }
        }
    }
}

/**
 * @brief  通过AT指令配置L101模块参数
 * @param  cmd 命令模式 1参数配置 2自由指令
 * @retval None
 */
void At_Handle(uint8_t cmd)
{
    Shell *sh = Shell_Object;
    char recive_data = '\0';

    if (cmd > FREE_MODE)
    {
        shellWriteString(sh, atText[UNKOWN_MODE]);
        return;
    }
    //    __set_PRIMASK(1); /* 禁止全局中断*/
    /*挂起mdbusHandle任务*/
    // osThreadSuspend(mdbusHandle);
    osThreadSuspendAll();
    /*停止发送定时器*/
    osTimerStop(Timer1Handle);
    //    __set_PRIMASK(0); /*  使能全局中断 */
    // __HAL_UART_DISABLE_IT(&huart1, UART_IT_IDLE);
    // HAL_NVIC_DisableIRQ(USART1_IRQn);
    shellWriteString(sh, atText[cmd]);
    cmd ? Free_Mode(sh, &recive_data) : Config_Mode(sh, &recive_data);
    /*打开发送定时器*/
    osTimerStart(Timer1Handle, MDTASK_SENDTIMES);
    /*恢复mdbusHandle任务*/
    // osThreadResume(mdbusHandle);
#if defined(USING_DEBUG)
    // shellPrint(sh, "portNVIC_INT_CTRL_REG = 0x%x\r\n", portNVIC_INT_CTRL_REG);
#endif
    // __set_PRIMASK(1); /* 禁止全局中断*/
    osThreadResumeAll();
    // __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
    // HAL_NVIC_EnableIRQ(USART1_IRQn);
    // __set_PRIMASK(0); /*  使能全局中断 */
}
#if defined(USING_DEBUG)
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_FUNC), at, At_Handle, config);
#endif
#endif
