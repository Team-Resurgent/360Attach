#include "stdafx.h"
#include "CciIo.h"
#include "CciStream.h"
#include "VTray.h"
#include "ConSign.h"
#include "MediaIdHook.h"

#include <cci/cci.h>

// Control requests share the worker queue with reads; they are told apart by
// IRP.Type, which is IO_TYPE_IRP for real reads.
#define CCIIO_REQ_MOUNT		1
#define CCIIO_REQ_UNMOUNT	2

#define XDVDFS_HEADER_OFFSET	0x10000

typedef struct _MOUNT_REQUEST {
	const CHAR * const * PartPaths;
	DWORD PartCount;
} MOUNT_REQUEST;

// Where the game partition can start inside an image, and where the DMI/SS
// sector pair sits for the full-disc layouts.
typedef struct _DISC_LAYOUT {
	const CHAR * Name;
	QWORD PartitionOffset;
	QWORD SecuritySectorOffset;		// 0 when the image carries none
} DISC_LAYOUT;

static const DISC_LAYOUT g_Layouts[] = {
	{ "full XGD2",			0xFD90000ULL, 0xFD8F000ULL },
	{ "full XGD3",			0x2080000ULL, 0x2077000ULL },
	{ "game partition",		0x0ULL,		  0x0ULL },
};

static const BYTE XdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";

static struct {
	HANDLE Thread;
	DWORD SpinLock;
	LIST_ENTRY Queue;
	KEVENT Wake;
} g_Worker;

static BOOL g_Started = FALSE;
static volatile BOOL g_Mounted = FALSE;
static DWORD g_BaseSector = 0;
static DWORD g_SectorCount = 0;
static CCI_MEDIA_INFO g_MediaInfo;

// Worker-only scratch space.
static BYTE g_Scratch[CCI_SECTOR_SIZE * 2];

static VOID ParseSecuritySectors( const BYTE * dmi, const BYTE * ss )
{
	// The media ID appears in both the DMI (at 0x20) and the SS (at 0x460);
	// only trust the pair when they agree and are not blank.
	BOOL blank = TRUE;
	for( int i = 0; i < 0x10; i++ )
	{
		if( dmi[0x20 + i] != 0x00 && dmi[0x20 + i] != 0xFF ) { blank = FALSE; break; }
	}
	if( blank || memcmp( dmi + 0x20, ss + 0x460, 0x10 ) != 0 ) return;

	memcpy( g_MediaInfo.MediaId, dmi + 0x20, sizeof(g_MediaInfo.MediaId) );
	memcpy( g_MediaInfo.LayerInfo, ss + 0x04, sizeof(g_MediaInfo.LayerInfo) );
	memcpy( &g_MediaInfo.HashTableLba, ss + 0x100, sizeof(g_MediaInfo.HashTableLba) );
	memcpy( g_MediaInfo.HashTableHash, ss + 0x108, sizeof(g_MediaInfo.HashTableHash) );
	g_MediaInfo.Valid = TRUE;
	g_MediaInfo.HasMediaId = TRUE;
}

static BYTE ToLowerAscii( BYTE c ) { return ( c >= 'A' && c <= 'Z' ) ? (BYTE)(c + 0x20) : c; }

static BOOL NameEquals( const BYTE * a, const char * b, DWORD len )
{
	for( DWORD i = 0; i < len; i++ )
		if( ToLowerAscii( a[i] ) != ToLowerAscii( (BYTE)b[i] ) ) return FALSE;
	return TRUE;
}

// Finds a file in the XDVDFS root directory by name (the entries form a binary
// tree of 4-byte-unit links, walked with a bounded stack). Returns its start
// sector, relative to the game partition, and size.
static BOOL FindRootFile( const char * name, DWORD nameLen, DWORD * outSector, DWORD * outSize )
{
	if( !NT_SUCCESS(CciStreamReadSectors( g_BaseSector + XDVDFS_HEADER_OFFSET / CCI_SECTOR_SIZE, 1, g_Scratch )) ) return FALSE;
	// XDVDFS fields are little-endian; this runs big-endian, so assemble by byte.
	const BYTE * h = g_Scratch;
	DWORD rootSector = h[0x14] | (h[0x15] << 8) | (h[0x16] << 16) | ((DWORD)h[0x17] << 24);
	DWORD rootSize = h[0x18] | (h[0x19] << 8) | (h[0x1A] << 16) | ((DWORD)h[0x1B] << 24);
	if( rootSize == 0 || rootSize > 0x100000 || rootSector >= g_SectorCount ) return FALSE;

	DWORD sectors = (rootSize + CCI_SECTOR_SIZE - 1) / CCI_SECTOR_SIZE;
	BYTE * dir = (BYTE *)ExAllocatePool( sectors * CCI_SECTOR_SIZE );
	WORD * stack = (WORD *)ExAllocatePool( (rootSize / 4 + 1) * sizeof(WORD) );
	BOOL found = FALSE;
	if( dir != NULL && stack != NULL && NT_SUCCESS(CciStreamReadSectors( g_BaseSector + rootSector, sectors, dir )) )
	{
		DWORD maxEntries = rootSize / 4, depth = 0, visited = 0;
		stack[depth++] = 0;
		while( depth > 0 && visited++ < maxEntries )
		{
			DWORD offset = (DWORD)stack[--depth] * 4;
			if( offset + 14 > rootSize ) continue;
			const BYTE * e = dir + offset;
			WORD left = (WORD)( e[0] | (e[1] << 8) );
			WORD right = (WORD)( e[2] | (e[3] << 8) );
			BYTE nlen = e[13];
			if( left == 0xFFFF || nlen == 0 ) continue;
			if( left != 0 && depth < maxEntries ) stack[depth++] = left;
			if( right != 0 && right != 0xFFFF && depth < maxEntries ) stack[depth++] = right;
			if( offset + 14 + nlen <= rootSize && nlen == nameLen && NameEquals( e + 14, name, nameLen ) )
			{
				*outSector = e[4] | (e[5] << 8) | (e[6] << 16) | ((DWORD)e[7] << 24);
				*outSize = e[8] | (e[9] << 8) | (e[10] << 16) | ((DWORD)e[11] << 24);
				found = TRUE;
				break;
			}
		}
	}
	if( stack != NULL ) ExFreePool( stack );
	if( dir != NULL ) ExFreePool( dir );
	return found;
}

// A trimmed image carries no security sectors, so when the full-disc parse did
// not find a media ID, take it from the disc's default.xex header instead (the
// XEX's own media ID, which a converted disc preserves). It is what a signed-in
// profile's content, update and licence checks compare against.
static VOID ReadDiscXexMediaId( VOID )
{
	if( g_MediaInfo.HasMediaId ) return;

	DWORD xexSector = 0, xexSize = 0;
	if( !FindRootFile( "default.xex", 11, &xexSector, &xexSize ) || xexSector >= g_SectorCount )
	{
		LOG( "no media ID: default.xex not found on disc" );
		return;
	}

	if( !NT_SUCCESS(CciStreamReadSectors( g_BaseSector + xexSector, 1, g_Scratch )) ) return;
	if( memcmp( g_Scratch, "XEX2", 4 ) != 0 ) { LOG( "no media ID: disc default.xex is not XEX2" ); return; }

	// Security info offset is big-endian; the media ID sits in its image header
	// (XEX_SECURITY_INFO + 8 for HV_IMAGE_INFO, + 0x138 for MediaID).
	const BYTE * h = g_Scratch;
	DWORD secInfo = ((DWORD)h[0x10] << 24) | (h[0x11] << 16) | (h[0x12] << 8) | h[0x13];
	DWORD mediaOff = secInfo + 0x8 + 0x138;
	DWORD need = mediaOff + sizeof(g_MediaInfo.MediaId);
	DWORD sectors = (need + CCI_SECTOR_SIZE - 1) / CCI_SECTOR_SIZE;
	if( sectors == 0 || sectors > 0x20 || xexSector + sectors > g_SectorCount ) return;

	BYTE * buf = (BYTE *)ExAllocatePool( sectors * CCI_SECTOR_SIZE );
	if( buf == NULL ) return;
	if( NT_SUCCESS(CciStreamReadSectors( g_BaseSector + xexSector, sectors, buf )) )
	{
		memcpy( g_MediaInfo.MediaId, buf + mediaOff, sizeof(g_MediaInfo.MediaId) );
		g_MediaInfo.HasMediaId = TRUE;
		LOG( "disc media ID from default.xex: %02X%02X%02X%02X...", buf[mediaOff], buf[mediaOff+1], buf[mediaOff+2], buf[mediaOff+3] );
	}
	ExFreePool( buf );
}

// Finds the XDVDFS header and records where the game partition begins.
static NTSTATUS DetectLayout( VOID )
{
	DWORD total = CciStreamSectorCount();
	for( int i = 0; i < ARRAYSIZE(g_Layouts); i++ )
	{
		const DISC_LAYOUT * layout = &g_Layouts[i];
		DWORD headerSector = (DWORD)((layout->PartitionOffset + XDVDFS_HEADER_OFFSET) / CCI_SECTOR_SIZE);
		if( headerSector >= total ) continue;
		if( !NT_SUCCESS(CciStreamReadSectors( headerSector, 1, g_Scratch )) ) continue;
		if( memcmp( g_Scratch, XdvdfsMagic, sizeof(XdvdfsMagic) - 1 ) != 0 ) continue;

		g_BaseSector = (DWORD)(layout->PartitionOffset / CCI_SECTOR_SIZE);
		g_SectorCount = total - g_BaseSector;
		if( layout->SecuritySectorOffset != 0 &&
			NT_SUCCESS(CciStreamReadSectors( (DWORD)(layout->SecuritySectorOffset / CCI_SECTOR_SIZE), 2, g_Scratch )) )
		{
			ParseSecuritySectors( g_Scratch, g_Scratch + CCI_SECTOR_SIZE );
		}
		LOG( "layout: %s, 0x%X sectors%s", layout->Name, g_SectorCount, g_MediaInfo.Valid ? ", security sectors found" : "" );
		return STATUS_SUCCESS;
	}
	LOG( "no XDVDFS volume found in image" );
	return STATUS_UNRECOGNIZED_MEDIA;
}

static VOID SetMounted( BOOL mounted )
{
	g_Mounted = mounted;
	__lwsync();
}

static DWORD g_ReadErrors = 0;

static NTSTATUS DoMount( const MOUNT_REQUEST * req )
{
	g_ReadErrors = 0;
	SetMounted( FALSE );
	memset( &g_MediaInfo, 0, sizeof(g_MediaInfo) );

	NTSTATUS status = CciStreamOpen( req->PartPaths, req->PartCount );
	if( NT_SUCCESS(status) ) status = DetectLayout();
	if( NT_SUCCESS(status) )
	{
		ConSignScan( g_BaseSector, g_SectorCount );
		ReadDiscXexMediaId();
		MediaIdHookInstall();
		SetMounted( TRUE );
		LOG( "mounted: %u sectors from partition sector 0x%X", g_SectorCount, g_BaseSector );
	}
	else
	{
		LOG( "mount failed 0x%08X (check the lines above for the step that failed)", status );
		CciStreamClose();
	}

	VTrayCycle();
	return status;
}

static NTSTATUS DoUnmount( VOID )
{
	SetMounted( FALSE );
	MediaIdHookRemove();
	memset( &g_MediaInfo, 0, sizeof(g_MediaInfo) );
	ConSignReset();
	CciStreamClose();
	VTrayCycle();
	return STATUS_SUCCESS;
}

static VOID DoRead( PIRP irp )
{
	PIO_STACK_LOCATION sloc = irp->Tail.Overlay.CurrentStackLocation;
	BYTE * dest = (BYTE *)(sloc->Parameters.Read.BufferOffset + sloc->Parameters.Read.ByteOffset.LowPart);
	DWORD sector = sloc->Parameters.Read.ByteOffset.HighPart;
	DWORD length = sloc->Parameters.Read.Length;
	DWORD whole = length / CCI_SECTOR_SIZE;
	DWORD tail = length % CCI_SECTOR_SIZE;

	NTSTATUS status = STATUS_NO_MEDIA_IN_DEVICE;
	if( g_Mounted )
	{
		if( sector >= g_SectorCount || whole + (tail ? 1 : 0) > g_SectorCount - sector )
		{
			status = STATUS_INVALID_PARAMETER;
		}
		else
		{
			status = CciStreamReadSectors( g_BaseSector + sector, whole, dest );
			if( NT_SUCCESS(status) && tail != 0 )
			{
				status = CciStreamReadSectors( g_BaseSector + sector + whole, 1, g_Scratch );
				if( NT_SUCCESS(status) ) memcpy( dest + whole * CCI_SECTOR_SIZE, g_Scratch, tail );
			}
			if( NT_SUCCESS(status) ) ConSignApply( sector, length, dest );
			else status = STATUS_IO_DEVICE_ERROR;
		}
	}

	if( !NT_SUCCESS(status) && g_ReadErrors++ < 16 )
		LOG( "read failed: sector 0x%X, %u bytes, status 0x%08X", sector, length, status );
	irp->IoStatus.Information = NT_SUCCESS(status) ? length : 0;
	irp->IoStatus.st.Status = status;
	IoCompleteRequest( irp, IO_CD_ROM_INCREMENT );
}

static DWORD WINAPI WorkerMain( PVOID context )
{
	for( ;; )
	{
		if( KeWaitForSingleObject( &g_Worker.Wake, Executive, UserMode, TRUE, NULL ) == STATUS_USER_APC )
			continue;

		for( ;; )
		{
			BYTE irql = KfAcquireSpinLock( &g_Worker.SpinLock );
			PLIST_ENTRY entry = RemoveHeadList( &g_Worker.Queue );
			KfReleaseSpinLock( &g_Worker.SpinLock, irql );
			if( entry == &g_Worker.Queue ) break;

			PIRP irp = CONTAINING_RECORD( entry, IRP, Tail.Overlay.DeviceListEntry );
			switch( irp->Type )
			{
			case IO_TYPE_IRP:
				DoRead( irp );
				break;
			case CCIIO_REQ_MOUNT:
				irp->IoStatus.st.Status = DoMount( (const MOUNT_REQUEST *)irp->UserBuffer );
				KeSetEvent( irp->UserEvent, 1, FALSE );
				break;
			case CCIIO_REQ_UNMOUNT:
				irp->IoStatus.st.Status = DoUnmount();
				KeSetEvent( irp->UserEvent, 1, FALSE );
				break;
			}
		}
	}
	return 0;
}

static VOID Enqueue( PIRP irp )
{
	BYTE irql = KfAcquireSpinLock( &g_Worker.SpinLock );
	InsertTailList( &g_Worker.Queue, &irp->Tail.Overlay.DeviceListEntry );
	KfReleaseSpinLock( &g_Worker.SpinLock, irql );
	KeSetEvent( &g_Worker.Wake, 1, FALSE );
}

// Runs a control request on the worker thread and waits for its result. File
// handles opened there belong to the system process, not the calling title.
static NTSTATUS RunOnWorker( SHORT type, PVOID buffer )
{
	IRP request;
	KEVENT done;
	memset( &request, 0, sizeof(request) );
	KeInitializeEvent( &done, EventSynchronizationObject, FALSE );
	request.Type = type;
	request.UserBuffer = buffer;
	request.UserEvent = &done;
	request.IoStatus.st.Status = STATUS_UNSUCCESSFUL;

	Enqueue( &request );
	KeWaitForSingleObject( &done, Executive, UserMode, FALSE, NULL );
	return request.IoStatus.st.Status;
}

VOID CciIoStartup( VOID )
{
	if( g_Started ) return;
	g_Started = TRUE;

	InitializeListHead( &g_Worker.Queue );
	KeInitializeEvent( &g_Worker.Wake, EventSynchronizationObject, FALSE );

	// System thread on the last hardware thread, which titles rarely saturate.
	// 32 KB leaves room for the console signing at mount, which runs on this thread.
	DWORD flags = EX_CREATE_FLAG_SYSTEM | EX_CREATE_FLAG_CORE5 | EX_CREATE_FLAG_PRIORITY1;
	if( NT_SUCCESS(ExCreateThread( &g_Worker.Thread, 0x8000, NULL, NULL, (LPTHREAD_START_ROUTINE)WorkerMain, NULL, flags )) )
		SetThreadPriority( g_Worker.Thread, THREAD_PRIORITY_TIME_CRITICAL );
	else
		LOG( "worker thread creation failed" );

	VTrayStartup();
}

NTSTATUS CciIoMount( const CHAR * const * partPaths, DWORD partCount )
{
	MOUNT_REQUEST req = { partPaths, partCount };
	return RunOnWorker( CCIIO_REQ_MOUNT, &req );
}

NTSTATUS CciIoUnmount( VOID )
{
	return RunOnWorker( CCIIO_REQ_UNMOUNT, NULL );
}

BOOL CciIoIsMounted( VOID )
{
	return g_Mounted;
}

DWORD CciIoSectorCount( VOID )
{
	return g_Mounted ? g_SectorCount : 0;
}

const CCI_MEDIA_INFO * CciIoMediaInfo( VOID )
{
	return &g_MediaInfo;
}

NTSTATUS CciIoQueueRead( PIRP irp )
{
	Enqueue( irp );
	return STATUS_PENDING;
}
