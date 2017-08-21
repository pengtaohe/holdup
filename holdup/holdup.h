#ifndef __HOLDUP_H__
#define __HOLDUP_H__

typedef struct
{
	void (*pfunc)( int, bcm_port_t, bcm_port_info_t *);
	int unit;
	bcm_port_t port;
	bcm_port_info_t info;
}HOLDUP_CONTEXT;

typedef struct
{
	ULONG ulHoldupStartTime;
	ULONG ulHoldupTimerId;
	ULONG ulHoldupSemBId;
	ULONG ulHoldupContextNeedRestroe;
	HOLDUP_CONTEXT stHoldupContext;
}HOLDUP_S;

typedef enum
{
	HOLDUP_STATUS_NULL,
	HOLDUP_STATUS_A,
	HOLDUP_STATUS_B
}HOLDUP_STATUS;

extern ULONG holdup(HOLDUP_CONTEXT * pContext);

#endif