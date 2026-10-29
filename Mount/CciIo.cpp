#include "stdafx.h"
#include "CciIo.h"
#include "CciStream.h"
#include "XexDevkit.h"
#include "VTray.h"

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

// XDVDFS volume descriptor fields and directory entry layout.
#define XDVDFS_ROOT_SECTOR_OFFSET	0x14
#define XDVDFS_ROOT_SIZE_OFFSET		0x18
#define XDVDFS_DIRENT_NAME_OFFSET	0x0E
#define XDVDFS_MAX_ROOT_SIZE		(64 * 1024)
#define XDVDFS_MAX_TREE_DEPTH		64

#define DISC_XEX_NAME				"default.xex"

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

static BYTE g_RootDir[XDVDFS_MAX_ROOT_SIZE];

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

static DWORD ReadLE32( const BYTE * p )
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static WORD ReadLE16( const BYTE * p )
{
	return (WORD)(p[0] | (p[1] << 8));
}

// Finds a file in the root directory of the mounted volume. Entries form a
// binary tree whose links are offsets in 4-byte units; walk all of it rather
// than rely on the tree's sort order.
static BOOL FindRootFile( const CHAR * name, DWORD * startSector, DWORD * size )
{
	if( !NT_SUCCESS(CciStreamReadSectors( g_BaseSector + XDVDFS_HEADER_OFFSET / CCI_SECTOR_SIZE, 1, g_Scratch )) )
		return FALSE;
	DWORD rootSector = ReadLE32( g_Scratch + XDVDFS_ROOT_SECTOR_OFFSET );
	DWORD rootSize = ReadLE32( g_Scratch + XDVDFS_ROOT_SIZE_OFFSET );
	DWORD rootSectors = (rootSize + CCI_SECTOR_SIZE - 1) / CCI_SECTOR_SIZE;
	if( rootSize == 0 || rootSize > sizeof(g_RootDir) || rootSector >= g_SectorCount || rootSectors > g_SectorCount - rootSector )
	{
		LOG( "root directory (sector 0x%X, 0x%X bytes) not supported", rootSector, rootSize );
		return FALSE;
	}
	if( !NT_SUCCESS(CciStreamReadSectors( g_BaseSector + rootSector, rootSectors, g_RootDir )) )
		return FALSE;

	size_t nameLen = strlen( name );
	DWORD stack[XDVDFS_MAX_TREE_DEPTH];
	DWORD depth = 0;
	stack[depth++] = 0;
	while( depth > 0 )
	{
		DWORD offset = stack[--depth];
		if( offset + XDVDFS_DIRENT_NAME_OFFSET > rootSize ) continue;
		const BYTE * entry = g_RootDir + offset;
		BYTE entryNameLen = entry[XDVDFS_DIRENT_NAME_OFFSET - 1];
		if( offset + XDVDFS_DIRENT_NAME_OFFSET + entryNameLen > rootSize ) continue;

		if( entryNameLen == nameLen && _strnicmp( (const CHAR *)entry + XDVDFS_DIRENT_NAME_OFFSET, name, nameLen ) == 0 )
		{
			*startSector = ReadLE32( entry + 4 );
			*size = ReadLE32( entry + 8 );
			return TRUE;
		}

		WORD left = ReadLE16( entry ), right = ReadLE16( entry + 2 );
		if( left != 0 && left != 0xFFFF && depth < XDVDFS_MAX_TREE_DEPTH ) stack[depth++] = (DWORD)left * 4;
		if( right != 0 && right != 0xFFFF && depth < XDVDFS_MAX_TREE_DEPTH ) stack[depth++] = (DWORD)right * 4;
	}
	return FALSE;
}

// On a devkit, resign the disc's default.xex into a header-sector buffer.
// Retail and RGH consoles mount the image unchanged.
static NTSTATUS PrepareDevkitXex( VOID )
{
	if( !XexDevkitIsConsoleDevkit() ) return STATUS_SUCCESS;

	DWORD start, size;
	if( !FindRootFile( DISC_XEX_NAME, &start, &size ) )
	{
		LOG( "devkit: %s not found in the root directory", DISC_XEX_NAME );
		return STATUS_OBJECT_NAME_NOT_FOUND;
	}
	if( start >= g_SectorCount )
	{
		LOG( "devkit: %s sector 0x%X is past the end of the partition", DISC_XEX_NAME, start );
		return STATUS_FILE_CORRUPT_ERROR;
	}
	return XexDevkitPatch( g_BaseSector + start, start, size );
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

	XexDevkitClear();

	NTSTATUS status = CciStreamOpen( req->PartPaths, req->PartCount );
	if( NT_SUCCESS(status) ) status = DetectLayout();
	if( NT_SUCCESS(status) ) status = PrepareDevkitXex();
	if( NT_SUCCESS(status) )
	{
		SetMounted( TRUE );
		LOG( "mounted: %u sectors from partition sector 0x%X", g_SectorCount, g_BaseSector );
	}
	else
	{
		LOG( "mount failed 0x%08X (check the lines above for the step that failed)", status );
		XexDevkitClear();
		CciStreamClose();
	}

	VTrayCycle();
	return status;
}

static NTSTATUS DoUnmount( VOID )
{
	SetMounted( FALSE );
	memset( &g_MediaInfo, 0, sizeof(g_MediaInfo) );
	XexDevkitClear();
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
			if( NT_SUCCESS(status) ) XexDevkitSplice( sector, length, dest );
			if( !NT_SUCCESS(status) ) status = STATUS_IO_DEVICE_ERROR;
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
	// 32 KB leaves room for the devkit RSA resign, which runs on this thread.
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
