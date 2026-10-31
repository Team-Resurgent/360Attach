#include "stdafx.h"
#include "MediaIdHook.h"
#include "CciIo.h"
#include "KernelHook.h"

#define XEKEYS_GET_MEDIA_ID_ORDINAL		690

typedef NTSTATUS (NTAPI * XE_KEYS_GET_MEDIA_ID)( PBYTE pMediaID, BOOL fCheckHvAuth );

static DWORD g_OriginalAddr = 0;

static NTSTATUS NTAPI GetMediaIdHook( PBYTE pMediaID, BOOL fCheckHvAuth )
{
	const CCI_MEDIA_INFO * media = CciIoMediaInfo();
	if( media->HasMediaId )
	{
		memcpy( pMediaID, media->MediaId, sizeof(media->MediaId) );
		return STATUS_SUCCESS;
	}
	// Nothing mounted with a known media ID: fall back to the original.
	if( g_OriginalAddr != 0 ) return ((XE_KEYS_GET_MEDIA_ID)g_OriginalAddr)( pMediaID, fCheckHvAuth );
	return XeKeysGetMediaID( pMediaID, fCheckHvAuth );
}

VOID MediaIdHookInstall( VOID )
{
	if( g_OriginalAddr != 0 ) return;				// already installed
	// Needs a media ID to serve (from security sectors or the disc XEX header).
	if( !CciIoMediaInfo()->HasMediaId ) return;

	g_OriginalAddr = KernelHookExport( XEKEYS_GET_MEDIA_ID_ORDINAL, (PVOID)GetMediaIdHook );
	if( g_OriginalAddr != 0 ) LOG( "XeKeysGetMediaID hooked; serving the image's media ID" );
}

VOID MediaIdHookRemove( VOID )
{
	if( g_OriginalAddr == 0 ) return;
	KernelUnhookExport( XEKEYS_GET_MEDIA_ID_ORDINAL, g_OriginalAddr );
	g_OriginalAddr = 0;
}
