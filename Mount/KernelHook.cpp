#include "stdafx.h"
#include "KernelHook.h"

// The debug monitor's memory write, resolved from xbdm.xex at run time. A
// devkit keeps the export table in a write-protected image page, so a direct
// store faults there and this is used instead; on a retail or RGH kernel xbdm
// is absent and a direct store reaches the page.
typedef HRESULT (*DM_SET_MEMORY)( LPVOID dest, DWORD size, LPCVOID src, LPDWORD written );
#define DM_SET_MEMORY_ORDINAL	40

static DM_SET_MEMORY g_DmSetMemory = (DM_SET_MEMORY)-1;		// -1: not yet resolved

static DM_SET_MEMORY DebugSetMemory( VOID )
{
	if( g_DmSetMemory == (DM_SET_MEMORY)-1 )
	{
		HANDLE xbdm = NULL;
		PVOID fn = NULL;
		g_DmSetMemory = NULL;
		if( XexGetModuleHandle( MODULE_XBDM, &xbdm ) == 0 && xbdm != NULL &&
			XexGetProcedureAddress( xbdm, DM_SET_MEMORY_ORDINAL, &fn ) == 0 )
		{
			g_DmSetMemory = (DM_SET_MEMORY)fn;
		}
	}
	return g_DmSetMemory;
}

static VOID WriteExportEntry( volatile DWORD * dest, DWORD value )
{
	DM_SET_MEMORY dm = DebugSetMemory();
	if( dm != NULL )
	{
		DWORD written = 0;
		dm( (LPVOID)dest, sizeof(DWORD), &value, &written );
	}
	else
	{
		*dest = value;
		__dcbst( 0, (void *)dest );
		__sync();
	}
}

// The kernel carries no PE-exports header field, so its export table is the one
// named in the security info's image header.
static PIMAGE_EXPORT_ADDRESS_TABLE KernelExportTable( VOID )
{
	HANDLE handle = NULL;
	if( XexGetModuleHandle( MODULE_KERNEL, &handle ) != 0 || handle == NULL ) return NULL;

	PIMAGE_XEX_HEADER header = (PIMAGE_XEX_HEADER)((PLDR_DATA_TABLE_ENTRY)handle)->XexHeaderBase;
	if( header == NULL ) return NULL;
	if( RtlImageXexHeaderField( header, XEX_HEADER_PE_EXPORTS ) != NULL ) return NULL;

	PXEX_SECURITY_INFO security = (PXEX_SECURITY_INFO)header->SecurityInfo;
	return (PIMAGE_EXPORT_ADDRESS_TABLE)security->ImageInfo.ExportTableAddress;
}

DWORD KernelHookExport( DWORD ordinal, PVOID replacement )
{
	if( replacement == NULL ) return 0;
	PIMAGE_EXPORT_ADDRESS_TABLE table = KernelExportTable();
	if( table == NULL )
	{
		LOG( "kernel export table not found; cannot hook ordinal %u", ordinal );
		return 0;
	}

	// Each entry is the export's offset from the module base; the base is the
	// image-base field scaled by 0x10000.
	DWORD base = table->ImageBaseAddress << 16;
	DWORD original = table->ordOffset[ordinal - 1] + base;
	WriteExportEntry( &table->ordOffset[ordinal - 1], (DWORD)replacement - base );
	return original;
}

VOID KernelUnhookExport( DWORD ordinal, DWORD original )
{
	if( original == 0 ) return;
	PIMAGE_EXPORT_ADDRESS_TABLE table = KernelExportTable();
	if( table == NULL ) return;

	DWORD base = table->ImageBaseAddress << 16;
	WriteExportEntry( &table->ordOffset[ordinal - 1], original - base );
}
