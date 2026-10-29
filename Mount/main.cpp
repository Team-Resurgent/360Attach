#include "stdafx.h"
#include "CciIo.h"
#include "DvdHook.h"

static RTL_CRITICAL_SECTION g_MountLock;
static BOOL g_Hooked = FALSE;

DWORD CciGetVersion( VOID )
{
	return CCIMOUNT_VERSION;
}

NTSTATUS CciMount( const CHAR * const * partPaths, DWORD partCount )
{
	if( partPaths == NULL || partCount == 0 || partCount > CCIMOUNT_MAX_PARTS )
		return STATUS_INVALID_PARAMETER;

	RtlEnterCriticalSection( &g_MountLock );
	NTSTATUS status = STATUS_SUCCESS;
	if( !g_Hooked )
	{
		CciIoStartup();
		status = DvdHookStartup();
		g_Hooked = NT_SUCCESS(status);
	}
	if( NT_SUCCESS(status) )
	{
		LOG( "mounting %s (%u part%s)", partPaths[0], partCount, partCount == 1 ? "" : "s" );
		status = CciIoMount( partPaths, partCount );
	}
	RtlLeaveCriticalSection( &g_MountLock );

	LOG( "mount returns 0x%08X", status );
	return status;
}

NTSTATUS CciUnmount( VOID )
{
	RtlEnterCriticalSection( &g_MountLock );
	NTSTATUS status = g_Hooked ? CciIoUnmount() : STATUS_SUCCESS;
	RtlLeaveCriticalSection( &g_MountLock );
	return status;
}

BOOL CciIsMounted( VOID )
{
	return CciIoIsMounted();
}

BOOL APIENTRY DllMain( HANDLE hModule, DWORD dwReason, LPVOID lpReserved )
{
	if( dwReason == DLL_PROCESS_ATTACH )
	{
		RtlInitializeCriticalSection( &g_MountLock );
		LOG( "loaded, version 0x%08X", CCIMOUNT_VERSION );
	}
	else if( dwReason == DLL_PROCESS_DETACH )
	{
		// a game may still be reading the disc; only drop the hook if nothing is mounted
		if( !CciIoIsMounted() ) DvdHookShutdown();
	}
	return TRUE;
}
