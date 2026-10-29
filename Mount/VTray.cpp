#include "stdafx.h"
#include "VTray.h"

#define SMC_MSG_TRAY_STATE	0x83

// Head of the kernel's SMC notification list.
static PLIST_ENTRY g_SmcListHead = NULL;

static VOID NullSmcNotify( PHAL_SMC_REGISTRATION registration, PSMC_MAILBOX_MESSAGE message )
{
}

// The list head is not exported. Registering at the lowest priority appends
// our entry at the tail, so its Flink is the head; then unregister again.
static VOID FindSmcListHead( VOID )
{
	HAL_SMC_REGISTRATION reg;
	memset( &reg, 0, sizeof(reg) );
	reg.NotificationRoutine = NullSmcNotify;
	HalRegisterSMCNotification( &reg, TRUE );
	g_SmcListHead = reg.ListEntry.Flink;
	HalRegisterSMCNotification( &reg, FALSE );
}

static VOID BroadcastTrayState( BYTE state )
{
	if( g_SmcListHead == NULL ) return;

	SMC_MAILBOX_MESSAGE message;
	memset( &message, 0, sizeof(message) );
	message.MessageCode = SMC_MSG_TRAY_STATE;
	message.PayloadAsUCHARs[0] = state;

	for( PLIST_ENTRY entry = g_SmcListHead->Flink; entry != g_SmcListHead; entry = entry->Flink )
	{
		PHAL_SMC_REGISTRATION reg = CONTAINING_RECORD( entry, HAL_SMC_REGISTRATION, ListEntry );
		((SMCNOTIFYROUTINE)reg->NotificationRoutine)( reg, &message );
	}
}

VOID VTrayStartup( VOID )
{
	FindSmcListHead();
}

VOID VTrayCycle( VOID )
{
	LOG( "virtual tray cycle" );
	BroadcastTrayState( SMC_TRAY_OPEN );
	Sleep( 200 );
	BroadcastTrayState( SMC_TRAY_CLOSE );
	Sleep( 200 );
}
