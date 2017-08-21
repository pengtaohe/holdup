/***************************************************************************************************
 * 文 件 名: holdup.c
 *
 * 功能描述: 实现状态震荡抑制。A / B 两种状态频繁切换时，在设定的周期内屏蔽状态上报。
 *
 * 修改记录:
 * 2017.8.17  hept 创建
 *
 ***************************************************************************************************/
#include "syscfg.h"
#include "vos_global.h"
#include "vos/vospubh/Vos_types.h"
#include "vos/vospubh/Vos_typevx.h"
#include "vos/vospubh/vos_assert.h"
#include "vos/vospubh/vos_sem.h"
#include "vos/vospubh/vos_que.h"
#include "vos/vospubh/vos_task.h"
#include "vos/vospubh/vos_timer.h"
#include "vos/vospubh/vos_byteorder.h"
#include "sys/main/sys_main.h"

#include "enet.h"
#include "pmux.h"
#include "bms_pub.h"
#include "bmsp/cpufc/bms_cpufc.h"
#include "bmsp/product_info/bms_product_info.h"

#include "bcm/port.h"

#include "holdup.h"

#define HOLDUP_INDEX_MAX	6

ULONG ulHoldupEnable = 0;
ULONG ulHoldupPeriod = 0; /* tick */
ULONG ulHoldupMqueId = 0;
ULONG ulHoldupTaskId = 0;

ULONG ulHoldupDebug = 0;
#define	HOLDUP_DEBUG_PRINTF	if(ulHoldupDebug) sys_console_printf

HOLDUP_S g_HoldupInfo[HOLDUP_INDEX_MAX];

#if 1 /* this interface need write by user */

extern void bcm_port_linkscan_callback( int unit, bcm_port_t port, bcm_port_info_t *info );

ULONG holdup_inject(int unit, bcm_port_t port, bcm_port_info_t *info)
{
	if( SWITCH_PORT_2_USER_PORT(unit, port) >= 1 && SWITCH_PORT_2_USER_PORT(unit, port) <= 6 )
	{
		HOLDUP_CONTEXT pContext;
		pContext.pfunc = bcm_port_linkscan_callback;
		pContext.unit = unit;
		pContext.port = port;
		pContext.info = *info;
			
		return holdup(&pContext);
	}
	else
	{
		return 0;
	}
}

VOID _holdup_save_context(ULONG ulIndex, HOLDUP_CONTEXT * pContext)
{
    g_HoldupInfo[ulIndex].stHoldupContext = *pContext;
}

VOID _holdup_erase_context(ULONG ulIndex)
{
    memset(&g_HoldupInfo[ulIndex].stHoldupContext, 0, sizeof(HOLDUP_CONTEXT));
}

VOID _holdup_restore_context(ULONG ulIndex)
{
    g_HoldupInfo[ulIndex].stHoldupContext.pfunc(
        g_HoldupInfo[ulIndex].stHoldupContext.unit,
        g_HoldupInfo[ulIndex].stHoldupContext.port,
        &g_HoldupInfo[ulIndex].stHoldupContext.info);

	g_HoldupInfo[ulIndex].ulHoldupStartTime = 0;
    g_HoldupInfo[ulIndex].ulHoldupTimerId = 0;
    g_HoldupInfo[ulIndex].ulHoldupContextNeedRestroe = 0;
}

HOLDUP_STATUS _holdup_parse_context_status(HOLDUP_CONTEXT * pContext)
{
    bcm_port_info_t info = pContext->info;

    if(1 == info.linkstatus)
    {
        return HOLDUP_STATUS_A;
    }
    else
    {
        return HOLDUP_STATUS_B;
    }
}

ULONG _holdup_parse_context_index(HOLDUP_CONTEXT * pContext)
{
    ULONG ulPort = SWITCH_PORT_2_USER_PORT(pContext->unit, pContext->port);
    ULONG ulIndex = ulPort - 1;

    HOLDUP_DEBUG_PRINTF("\r\nunit:%d, port%d, %s\r\n",
                       pContext->unit, pContext->port, (1==pContext->info.linkstatus)?"up":"down");

    if(ulIndex >= HOLDUP_INDEX_MAX)
    {
        VOS_ASSERT(0);
        return HOLDUP_INDEX_MAX;
    }

    return ulIndex;
}

#endif

HOLDUP_STATUS _holdup_start_status(ULONG ulIndex, HOLDUP_STATUS status)
{
    static HOLDUP_STATUS StartStatus[HOLDUP_INDEX_MAX];
    if(HOLDUP_STATUS_NULL != status)
    {
        StartStatus[ulIndex] = status;
    }

    return StartStatus[ulIndex];
}

ULONG _holdup_is_new_status(ULONG ulIndex, HOLDUP_STATUS status)
{
    if(_holdup_start_status(ulIndex, HOLDUP_STATUS_NULL) != status)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

ULONG holdup_get_enable()
{
    return ulHoldupEnable;
}

VOID holdup_set_enable(ULONG ulEnable)
{
    ulHoldupEnable = ulEnable;
}

ULONG holdup_get_period()
{
    return ulHoldupPeriod;
}

VOID holdup_set_period(ULONG ulPeriod)
{
    ulHoldupPeriod = ulPeriod;
}

DEFUN( holdup_show_func,
       holdup_show_cmd,
       "show hold-up [config|status]",
       "Show\n"
       "Show hold-up config\n")
{
    ULONG i;

    if(0 == VOS_StrCmp(argv[0], "config"))
    {
        vty_out(vty, "Status: %s\r\n", (1 == holdup_get_enable())? "enable" : "disable");
        vty_out(vty, "Period: %d\r\n", holdup_get_period());
    }
    else if(0 == VOS_StrCmp(argv[0], "status"))
    {
        vty_out(vty, "Index   Status  Time(ms)\r\n");
        for(i = 0; i < HOLDUP_INDEX_MAX; i++)
        {
            if(g_HoldupInfo[i].ulHoldupContextNeedRestroe)
            {
                vty_out(vty, "%-8d%-8s%-8d\r\n", i+1,
                        (1 == g_HoldupInfo[i].stHoldupContext.info.linkstatus) ? "up" : "down",
                        g_HoldupInfo[i].ulHoldupStartTime + holdup_get_period() - VOS_GetTick());
            }
            else
            {
                vty_out(vty, "%-8d--      --      \r\n", i+1);
            }
        }
    }

    return CMD_SUCCESS;
}

DEFUN( holdup_enable_func,
       holdup_enable_cmd,
       "hold-up [enable|disable]",
       "config hold-up\n"
       "enable\n"
       "disable\n"
     )
{
    if(0 == VOS_StrCmp( "enable", ( CHAR* ) argv[ 0 ] ) )
    {
        holdup_set_enable(1);
    }
    else if(0 == VOS_StrCmp( "disable", ( CHAR* ) argv[ 0 ] ))
    {
        holdup_set_enable(0);
    }

    return CMD_SUCCESS;
}

DEFUN( holdup_period_func,
       holdup_period_cmd,
       "hold-up period <time>",
       "config hold-up\n"
       "config hold-up period\n"
       "(time*10ms)\n"
     )
{
    ULONG ulPeriod = VOS_AtoL(argv[0]);

    holdup_set_period(ulPeriod);

    return CMD_SUCCESS;
}

DEFUN( holdup_debug_enable_func,
       holdup_debug_enable_cmd,
       "hold-up debug [enable|disable]",
       "config hold-up\n"
       "config hold-up debug\n"
       "enable\n"
       "disable\n"
     )
{
    if(0 == VOS_StrCmp( "enable", ( CHAR* ) argv[ 0 ] ) )
    {
        ulHoldupDebug = 1;
    }
    else if(0 == VOS_StrCmp( "disable", ( CHAR* ) argv[ 0 ] ))
    {
        ulHoldupDebug = 0;
    }

    return CMD_SUCCESS;
}

VOID holdup_cmd_init()
{
    install_element(CONFIG_NODE, &holdup_show_cmd);
    install_element(CONFIG_NODE, &holdup_enable_cmd);
    install_element(CONFIG_NODE, &holdup_period_cmd);
	install_element(CONFIG_NODE, &holdup_debug_enable_cmd);
}

VOID holdup_timer_callback(ULONG ulIndex)
{
    ULONG ulRet;
    ULONG aulQueMsg[4] = {0};
    aulQueMsg[ 0 ] = MODULE_DEVSM;	/* module */
    aulQueMsg[ 1 ] = 0;				/* msg type */
    aulQueMsg[ 2 ] = ulIndex;		/* data1 */
    aulQueMsg[ 3 ] = 0;				/* data2 */

    if( g_HoldupInfo[ulIndex].ulHoldupContextNeedRestroe )
    {
		HOLDUP_DEBUG_PRINTF("\r\n %%info: hold-up timeout, QueSend, Index:%d.\r\n", ulIndex);
        ulRet = VOS_QueSend( ulHoldupMqueId, aulQueMsg, NO_WAIT, MSG_PRI_NORMAL );
        if ( ulRet != VOS_OK )
        {
            VOS_ASSERT(0);
        }
    }
    else
    {
		HOLDUP_DEBUG_PRINTF("\r\n %%info: hold-up timeout, SemGive, Index:%d.\r\n", ulIndex);
		g_HoldupInfo[ulIndex].ulHoldupStartTime = 0;
	    g_HoldupInfo[ulIndex].ulHoldupTimerId = 0;
	    g_HoldupInfo[ulIndex].ulHoldupContextNeedRestroe = 0;
		VOS_SemGive(g_HoldupInfo[ulIndex].ulHoldupSemBId);
        return;
    }
}


/***************************************************************************************************
 * 函 数 名: holdup
 * 功能描述: 提供给其它模块的震荡抑制接口
 * 修改记录: 2017.8.18  hept 创建
 * 入口参数: pContext - 接口调用处的上下文环境，由调用者封装
 * 出口参数: 无
 * 返 回 值: 1: hold-up  0: continue
 ***************************************************************************************************/
ULONG holdup(HOLDUP_CONTEXT * pContext)
{
    ULONG ulIndex, ulCurrentTime, ulHoldupPeriod;
    ULONG *pHoldupStartTime = NULL;
    HOLDUP_STATUS eHoldupStatus;

    if(NULL == pContext)
    {
        return 0;
    }
	
	/* hold-up restore, don't hold-up  */
	if(VOS_GetCurrentTask() == ulHoldupTaskId)
	{
		return 0;
	}

	if( !holdup_get_enable() )
	{
		return 0;
	}

    ulIndex = _holdup_parse_context_index(pContext);
    if(ulIndex >= HOLDUP_INDEX_MAX)
    {
        return 0;
    }

    eHoldupStatus = _holdup_parse_context_status(pContext);
    pHoldupStartTime = &g_HoldupInfo[ulIndex].ulHoldupStartTime;
    ulCurrentTime = VOS_GetTick();
    ulHoldupPeriod = holdup_get_period();

    /* not hold-up time, start */
    if( 0 == *pHoldupStartTime )
    {
		HOLDUP_DEBUG_PRINTF("\r\n %%info: not hold-up time, start, Index:%d.\r\n", ulIndex);
		VOS_SemTake(g_HoldupInfo[ulIndex].ulHoldupSemBId, WAIT_FOREVER);
        *pHoldupStartTime = ulCurrentTime;
        _holdup_start_status(ulIndex, eHoldupStatus);
        g_HoldupInfo[ulIndex].ulHoldupTimerId = VOS_TimerCreate( MODULE_DEVSM, NULL, ulHoldupPeriod*10,
                                                holdup_timer_callback, (void*)ulIndex, VOS_TIMER_NO_LOOP);

        return 0;
    }
    /* hold-up time, save context */
    else if(ulCurrentTime < *pHoldupStartTime + ulHoldupPeriod)
    {
        if( _holdup_is_new_status(ulIndex, eHoldupStatus) )
        {
			HOLDUP_DEBUG_PRINTF("\r\n %%info: hold-up time, save context, Index:%d.\r\n", ulIndex);
            g_HoldupInfo[ulIndex].ulHoldupContextNeedRestroe = 1;
            _holdup_save_context(ulIndex, pContext);
        }
        else
        {
			HOLDUP_DEBUG_PRINTF("\r\n %%info: hold-up time, erase context, Index:%d.\r\n", ulIndex);
            g_HoldupInfo[ulIndex].ulHoldupContextNeedRestroe = 0;
            _holdup_erase_context(ulIndex);
        }

		return 1;
    }
	/* last hold-up timeout but not release, wait to start */
    else if(ulCurrentTime >= *pHoldupStartTime + ulHoldupPeriod)
    {
		HOLDUP_DEBUG_PRINTF("\r\n %%info: last hold-up timeout but not release, wait to start, Index:%d.\r\n", ulIndex);
        VOS_SemTake(g_HoldupInfo[ulIndex].ulHoldupSemBId, WAIT_FOREVER);
        *pHoldupStartTime = ulCurrentTime;
        _holdup_start_status(ulIndex, eHoldupStatus);
        g_HoldupInfo[ulIndex].ulHoldupTimerId = VOS_TimerCreate( MODULE_DEVSM, NULL, ulHoldupPeriod*10,
                                                holdup_timer_callback, (void*)ulIndex, VOS_TIMER_NO_LOOP);

        return 0;
    }
}

VOID holdup_task()
{
    ULONG ulRcvMsg[4] = {0};
	ULONG ulIndex = 0;

    while( 1 )
    {
        if ( VOS_ERROR == VOS_QueReceive( ulHoldupMqueId, ulRcvMsg, WAIT_FOREVER ) )
        {
            VOS_ASSERT(0);
            continue;
        }
		
		ulIndex = ulRcvMsg[2];
		HOLDUP_DEBUG_PRINTF("\r\n %%info: hold-up timeout, restore context and SemGive, Index:%d.\r\n", ulIndex);
		_holdup_restore_context(ulIndex);
		VOS_SemGive(g_HoldupInfo[ulIndex].ulHoldupSemBId);
    }
}

VOID holdup_data_init()
{
    memset(g_HoldupInfo, 0, sizeof(g_HoldupInfo));
}

VOID holdup_init()
{
	ULONG ulIndex;
	
    holdup_data_init();
    holdup_cmd_init();

	for(ulIndex = 0; ulIndex < HOLDUP_INDEX_MAX; ulIndex++)
	{
		g_HoldupInfo[ulIndex].ulHoldupSemBId = (ULONG)VOS_SemBCreate( VOS_SEM_Q_FIFO, VOS_SEM_FULL );
		if( !g_HoldupInfo[ulIndex].ulHoldupSemBId )
		{
			VOS_ASSERT( 0 );
			return;
		}
	}
	
    ulHoldupMqueId = VOS_QueCreate(100, VOS_MSG_Q_FIFO);
    if( !ulHoldupMqueId )
    {
        VOS_ASSERT( 0 );
        return;
    }
    ulHoldupTaskId = VOS_TaskCreate( "tHoldup", TASK_PRIORITY_HIGHEST, (VOS_TASK_ENTRY)holdup_task, NULL );

    VOS_QueBindTask( (VOS_HANDLE)ulHoldupTaskId, ulHoldupMqueId );
}

