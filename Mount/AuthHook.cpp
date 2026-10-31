#include "stdafx.h"
#include "AuthHook.h"
#include "ImportHook.h"

// XAM asks the kernel, through this import, whether a launching title carries a
// given privilege. XexGetProcedureAddress resolves the real routine so the hook
// can answer every other privilege truthfully.
#define XEX_CHECK_PRIVILEGE_ORDINAL		404

typedef BOOL (*XEX_CHECK_PRIVILEGE)( DWORD privilege );

static IMPORT_HOOK g_Hook = { 0, 0, 0, { 0, 0, 0, 0 } };
static XEX_CHECK_PRIVILEGE g_Real = NULL;

// A title that requires AP25 media authentication cannot satisfy it from a
// virtual disc; report just that privilege as absent so XAM skips the check,
// and pass everything else to the real routine unchanged.
static BOOL XexCheckExecutablePrivilegeHook( DWORD privilege )
{
	if( privilege == XEX_PRIVILEGE_AUTHENTICATION_EX_REQUIRED ) return FALSE;
	if( g_Real != NULL ) return g_Real( privilege );
	return FALSE;
}

VOID AuthHookInstall( VOID )
{
	if( g_Hook.ThunkAddr != 0 ) return;				// already installed

	HANDLE kernel = NULL;
	PVOID fn = NULL;
	if( XexGetModuleHandle( MODULE_KERNEL, &kernel ) == 0 && kernel != NULL &&
		XexGetProcedureAddress( kernel, XEX_CHECK_PRIVILEGE_ORDINAL, &fn ) == 0 )
	{
		g_Real = (XEX_CHECK_PRIVILEGE)fn;
	}

	if( ImportHookInstall( MODULE_XAM, MODULE_KERNEL, XEX_CHECK_PRIVILEGE_ORDINAL, (PVOID)XexCheckExecutablePrivilegeHook, &g_Hook ) )
		LOG( "XexCheckExecutablePrivilege hooked; AP25 media authentication bypassed for the mounted disc" );
	else
		LOG( "XexCheckExecutablePrivilege not hooked (import not found in xam)" );
}

VOID AuthHookRemove( VOID )
{
	if( g_Hook.ThunkAddr == 0 ) return;
	ImportHookRemove( &g_Hook );
	g_Real = NULL;
}
