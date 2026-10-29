#include "stdafx.h"
#include "DvdHook.h"
#include "CciIo.h"

#include <cci/cci.h>

// CdRom0 device-control codes the kernel and XAM send while probing a disc.
#define IOCTL_CDROM_CHECK_VERIFY			0x24800
#define IOCTL_CDROM_GET_DRIVE_GEOMETRY		0x2404C
#define IOCTL_DVD_READ_STRUCTURE			0x335142
#define IOCTL_SATA_AUTHENTICATE				0x24080
#define IOCTL_SATA_SET_SPINDLE_SPEED		0x24084
#define IOCTL_SATA_XGD2_GET_TEST_INFO		0x24090
#define IOCTL_SATA_REAUTHENTICATE			0x24094
#define IOCTL_SATA_CLEAR_AUTH_STATE			0x24098
#define IOCTL_SATA_DVD_AUTH_EX_CHALLENGE	0x240A0
#define IOCTL_SATA_GET_SPINDLE_SPEED		0x240A8
#define IOCTL_SATA_SET_BOOT_PERF_STAT		0x240B8
#define IOCTL_SATA_UNINTERRUPTIBLE_READS	0x240C4
#define IOCTL_SATA_X360_MEDIA				0x240CC
#define IOCTL_SATA_FIRMWARE_CHECK			0x240D0
#define IOCTL_SATA_STANDBY					0x240E0
#define IOCTL_SATA_TEST_UNIT_READY			0x240E4

#define XEKEY_LAST_DISC_AUTH_TIME	0x101C
#define FAKE_DISC_AUTH_TIME			0x00000C40	// a plausible tick count for a real auth
#define X360_MEDIA_GAME_DISC		3
#define AUTH_EX_RESULT				2

static NTSTATUS HookRead( PVOID deviceObject, PVOID irp );
static NTSTATUS HookDeviceControl( PVOID deviceObject, PVOID irp );
static VOID		HookStartIo( PVOID deviceObject, PVOID irp );

static PDEVICE_OBJECT g_CdRom = NULL;
static PDRIVER_OBJECT g_OrigDriverPtr = NULL;
static DRIVER_OBJECT g_OrigDriver;
static BOOL g_HasDrive = FALSE;
static DVD_SPINDLE_SPEED_INFO g_SpindleSpeed = { 0, 3, 4, 0 };

static DRIVER_OBJECT g_HookDriver = {
	(DRIVERVOID) HookStartIo,				// DriverStartIo
				 NULL,						// DriverDeleteDevice
				 NULL,						// DriverDismountVolume
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObCreate
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObClose
	(DRIVERLONG) HookRead,					// DObRead
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObWrite
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObQueryInfo
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObSetInfo
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObFlushBuffers
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObQueryVolumeInfo
	(DRIVERLONG) IoInvalidDeviceRequest,	// DObDirectoryControl
	(DRIVERLONG) HookDeviceControl,			// DObDeviceControl
	(DRIVERLONG) IoInvalidDeviceRequest		// DObCleanup
};

// Completes an IRP answered without touching the image. Callers block on
// UserEvent and read UserIosb, so both are filled before completion.
static NTSTATUS CompleteInline( PIRP irp, NTSTATUS status )
{
	irp->IoStatus.st.Status = status;
	if( irp->UserIosb != NULL )
	{
		irp->UserIosb->st.Status = status;
		irp->UserIosb->Information = irp->IoStatus.Information;
	}
	if( irp->UserEvent != NULL ) KeSetEvent( irp->UserEvent, 0, FALSE );
	IoCompleteRequest( irp, IO_NO_INCREMENT );
	return status;
}

static NTSTATUS GetXgd2TestInfo( PIRP irp, DWORD outLen )
{
	if( outLen < 8 ) return CompleteInline( irp, STATUS_BUFFER_TOO_SMALL );

	const CCI_MEDIA_INFO * media = CciIoMediaInfo();
	PXGD2_GET_DRIVE_INFO info = (PXGD2_GET_DRIVE_INFO)irp->UserBuffer;
	info->HashTableIndexLBA = media->HashTableLba;
	info->BCADescriptor = 0;
	info->CacheFlags = 3;
	irp->IoStatus.Information = 8;
	if( outLen >= sizeof(XGD2_GET_TEST_INFO) )
	{
		info->LastDiscAuthTime = FAKE_DISC_AUTH_TIME;
		irp->IoStatus.Information = sizeof(XGD2_GET_TEST_INFO);
	}
	if( outLen >= sizeof(XGD2_GET_DRIVE_INFO) )
	{
		// LayerInfo holds the start PSN, end PSN and layer 0 end PSN; layer 1
		// runs from the bit-inverted layer 0 end to the end PSN.
		info->Layer0Length = media->LayerInfo[2] - media->LayerInfo[0] + 1;
		info->Layer1Length = media->LayerInfo[1] - ((~(media->LayerInfo[2] + 1)) & 0xFFFFFF);
		memcpy( info->HashValueOfTable, media->HashTableHash, sizeof(info->HashValueOfTable) );
		irp->IoStatus.Information = sizeof(XGD2_GET_DRIVE_INFO);
	}
	return CompleteInline( irp, STATUS_SUCCESS );
}

static NTSTATUS MountedRead( PIRP irp )
{
	PIO_STACK_LOCATION sloc = irp->Tail.Overlay.CurrentStackLocation;
	LONGLONG offset = sloc->Parameters.Read.ByteOffset.QuadPart;
	DWORD length = sloc->Parameters.Read.Length;
	LONGLONG size = (LONGLONG)CciIoSectorCount() * CCI_SECTOR_SIZE;

	// Cached reads arrive as whole, aligned sectors; anything else is a caller bug.
	if( (sloc->Flags & IRP_NOCACHE) == 0 &&
		( ((DWORD)irp->UserBuffer & 1) != 0 || (sloc->Parameters.Read.BufferOffset & 1) != 0 ||
		  (length & (CCI_SECTOR_SIZE - 1)) != 0 || (offset & (CCI_SECTOR_SIZE - 1)) != 0 ||
		  offset + length > size ) )
	{
		LOG( "rejected read at 0x%I64X, 0x%X bytes", offset, length );
		irp->IoStatus.Information = 0;
		irp->IoStatus.st.Status = STATUS_INVALID_PARAMETER;
		IoCompleteRequest( irp, IO_NO_INCREMENT );
		return STATUS_INVALID_PARAMETER;
	}
	if( length == 0 )
	{
		irp->IoStatus.Information = 0;
		irp->IoStatus.st.Status = STATUS_SUCCESS;
		IoCompleteRequest( irp, IO_NO_INCREMENT );
		return STATUS_SUCCESS;
	}

	// Repack the stack location for the worker: the sector in HighPart and the
	// destination as BufferOffset + LowPart. Uncached reads already carry the
	// destination in BufferOffset; cached ones give an offset into UserBuffer.
	sloc->Parameters.Read.ByteOffset.HighPart = (DWORD)(offset / CCI_SECTOR_SIZE);
	if( sloc->Flags & IRP_NOCACHE )
	{
		sloc->Parameters.Read.ByteOffset.LowPart = 0;
	}
	else
	{
		sloc->Parameters.Read.ByteOffset.LowPart = sloc->Parameters.Read.BufferOffset;
		sloc->Parameters.Read.BufferOffset = (DWORD)irp->UserBuffer;
	}
	sloc->Flags = 0;
	sloc->Control |= SL_PENDING_RETURNED;
	irp->IoStatus.Information = 0;
	return CciIoQueueRead( irp );
}

static NTSTATUS MountedDeviceControl( PIRP irp )
{
	PIO_STACK_LOCATION sloc = irp->Tail.Overlay.CurrentStackLocation;
	DWORD code = sloc->Parameters.DeviceIoControl.IoControlCode;
	DWORD inLen = sloc->Parameters.DeviceIoControl.InputBufferLength;
	DWORD outLen = sloc->Parameters.DeviceIoControl.OutputBufferLength;
	PVOID in = sloc->Parameters.DeviceIoControl.InputBuffer;
	PVOID out = irp->UserBuffer;

	switch( code )
	{
	case IOCTL_CDROM_CHECK_VERIFY:
		return CompleteInline( irp, STATUS_SUCCESS );

	case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
	{
		PDISK_GEOMETRY geo = (PDISK_GEOMETRY)out;
		geo->Sectors = CciIoSectorCount();
		geo->BytesPerSector = CCI_SECTOR_SIZE;
		irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
		return CompleteInline( irp, STATUS_SUCCESS );
	}

	case IOCTL_DVD_READ_STRUCTURE:
	{
		// Only the physical format descriptor is asked for, when the loader
		// works out which media types the inserted disc satisfies.
		PDVD_READ_STRUCTURE request = (PDVD_READ_STRUCTURE)in;
		if( request->Format != DvdPhysicalDescriptor ) break;
		BYTE * pfi = (BYTE *)out;
		pfi[0] = 0x08;
		pfi[1] = 0x02;
		pfi[6] = 0x02;		// low nibble 2: a data disc rather than a video one
		return CompleteInline( irp, STATUS_SUCCESS );
	}

	case IOCTL_SATA_SET_SPINDLE_SPEED:
	{
		if( inLen != 4 ) return CompleteInline( irp, STATUS_INVALID_PARAMETER );
		DWORD speed = *(DWORD *)in;
		irp->IoStatus.Information = 0;
		if( outLen >= 4 )
		{
			*(DWORD *)out = g_SpindleSpeed.FastestSpeed;
			irp->IoStatus.Information = 4;
		}
		if( speed > 3 ) return CompleteInline( irp, STATUS_INVALID_PARAMETER );
		g_SpindleSpeed.CurrentSpeed = speed;
		g_SpindleSpeed.DesiredSpeed = speed;
		return CompleteInline( irp, STATUS_SUCCESS );
	}

	case IOCTL_SATA_GET_SPINDLE_SPEED:
		if( outLen != sizeof(DVD_SPINDLE_SPEED_INFO) ) return CompleteInline( irp, STATUS_BUFFER_TOO_SMALL );
		memcpy( out, &g_SpindleSpeed, sizeof(DVD_SPINDLE_SPEED_INFO) );
		irp->IoStatus.Information = sizeof(DVD_SPINDLE_SPEED_INFO);
		return CompleteInline( irp, STATUS_SUCCESS );

	case IOCTL_SATA_UNINTERRUPTIBLE_READS:
	case IOCTL_SATA_SET_BOOT_PERF_STAT:
	case IOCTL_SATA_CLEAR_AUTH_STATE:
	case IOCTL_SATA_STANDBY:
	case IOCTL_SATA_TEST_UNIT_READY:
		return CompleteInline( irp, STATUS_SUCCESS );

	case IOCTL_SATA_FIRMWARE_CHECK:
		return CompleteInline( irp, STATUS_NOT_SUPPORTED );

	case IOCTL_SATA_X360_MEDIA:
		if( outLen < 4 ) return CompleteInline( irp, STATUS_BUFFER_TOO_SMALL );
		*(DWORD *)out = X360_MEDIA_GAME_DISC;
		irp->IoStatus.Information = 4;
		return CompleteInline( irp, STATUS_SUCCESS );

	case IOCTL_SATA_AUTHENTICATE:
	case IOCTL_SATA_REAUTHENTICATE:
	{
		DWORD authTime = FAKE_DISC_AUTH_TIME;
		XeKeysSetKey( XEKEY_LAST_DISC_AUTH_TIME, &authTime, sizeof(authTime) );
		return CompleteInline( irp, STATUS_SUCCESS );
	}

	case IOCTL_SATA_XGD2_GET_TEST_INFO:
		return GetXgd2TestInfo( irp, outLen );

	case IOCTL_SATA_DVD_AUTH_EX_CHALLENGE:
		if( out == NULL || outLen != 4 ) return CompleteInline( irp, STATUS_INVALID_PARAMETER );
		*(DWORD *)out = AUTH_EX_RESULT;
		irp->IoStatus.Information = 4;
		return CompleteInline( irp, STATUS_SUCCESS );
	}

	LOG( "unhandled ioctl 0x%X", code );
	return CompleteInline( irp, STATUS_INVALID_DEVICE_REQUEST );
}

static NTSTATUS FailIrp( PIRP irp, NTSTATUS status )
{
	irp->IoStatus.Information = 0;
	irp->IoStatus.st.Status = status;
	IoCompleteRequest( irp, IO_NO_INCREMENT );
	return status;
}

static VOID HookStartIo( PVOID deviceObject, PVOID irp )
{
	if( g_HasDrive )
		((DRIVERVOID)g_OrigDriver.DriverStartIo)( deviceObject, irp );
	else
		FailIrp( (PIRP)irp, STATUS_INVALID_DEVICE_REQUEST );
}

static NTSTATUS HookRead( PVOID deviceObject, PVOID irp )
{
	if( CciIoIsMounted() ) return MountedRead( (PIRP)irp );
	if( g_HasDrive ) return g_OrigDriver.dObFuns.DObRead( deviceObject, irp );
	return FailIrp( (PIRP)irp, STATUS_INVALID_DEVICE_REQUEST );
}

static NTSTATUS HookDeviceControl( PVOID deviceObject, PVOID irp )
{
	if( CciIoIsMounted() ) return MountedDeviceControl( (PIRP)irp );
	if( g_HasDrive ) return g_OrigDriver.dObFuns.DObDeviceControl( deviceObject, irp );

	PIRP pirp = (PIRP)irp;
	if( pirp->Tail.Overlay.CurrentStackLocation->Parameters.DeviceIoControl.IoControlCode == IOCTL_CDROM_CHECK_VERIFY )
		return CompleteInline( pirp, STATUS_NO_MEDIA_IN_DEVICE );
	return FailIrp( pirp, STATUS_INVALID_DEVICE_REQUEST );
}

// \Device\Cdrom0 cannot be referenced by name while the drive is absent, but
// a symbolic link to it still resolves to the device object.
static PDEVICE_OBJECT FindCdRomDevice( VOID )
{
	PDEVICE_OBJECT device = NULL;
	ANSI_STRING linkName, targetName;
	RtlInitAnsiString( &linkName, "\\System??\\CciMountCdRom:" );
	RtlInitAnsiString( &targetName, "\\Device\\Cdrom0" );

	if( ObCreateSymbolicLink( &linkName, &targetName ) < 0 ) return NULL;

	PVOID link = NULL;
	NTSTATUS status = ObReferenceObjectByName( &linkName, 0, ObSymbolicLinkObjectType, NULL, &link );
	if( NT_SUCCESS(status) )
	{
		device = (PDEVICE_OBJECT)((POBJECT_SYMBOLIC_LINK)link)->LinkTargetObject;
		ObDereferenceObject( link );
	}
	else
	{
		LOG( "could not reference \\Device\\Cdrom0: 0x%08X", status );
	}
	ObDeleteSymbolicLink( &linkName );
	return device;
}

NTSTATUS DvdHookStartup( VOID )
{
	if( g_CdRom != NULL ) return STATUS_SUCCESS;

	PDEVICE_OBJECT device = FindCdRomDevice();
	if( device == NULL ) return STATUS_NO_SUCH_DEVICE;

	BYTE irql = IoAcquireDeviceObjectLock();
	g_OrigDriverPtr = device->DriverObject;
	memcpy( &g_OrigDriver, device->DriverObject, sizeof(DRIVER_OBJECT) );
	device->DriverObject = &g_HookDriver;
	// A device that never finished initializing has no drive behind it; clear
	// the flag so opens are let through to the hook.
	g_HasDrive = (device->Flags & DO_DEVICE_INITIALIZING) == 0;
	device->Flags &= ~DO_DEVICE_INITIALIZING;
	IoReleaseDeviceObjectLock( irql );
	g_CdRom = device;

	LOG( "CdRom0 hooked, physical drive %s", g_HasDrive ? "present" : "absent" );
	return STATUS_SUCCESS;
}

VOID DvdHookShutdown( VOID )
{
	if( g_CdRom == NULL ) return;
	BYTE irql = IoAcquireDeviceObjectLock();
	g_CdRom->DriverObject = g_OrigDriverPtr;
	IoReleaseDeviceObjectLock( irql );
	g_CdRom = NULL;
}
