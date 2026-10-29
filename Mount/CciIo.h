// Virtual disc state and the I/O worker. Read IRPs from the CdRom0 hook are
// queued here and served on a system thread from the mounted CCI.
#ifndef __CCIIO_H
#define __CCIIO_H

// Security-sector details of a full (non-trimmed) disc image.
typedef struct _CCI_MEDIA_INFO {
	BOOL Valid;
	BYTE MediaId[0x10];
	DWORD LayerInfo[3];			// start, end, layer 0 end PSNs
	DWORD HashTableLba;
	BYTE HashTableHash[0x14];
} CCI_MEDIA_INFO;

VOID	 CciIoStartup( VOID );
NTSTATUS CciIoMount( const CHAR * const * partPaths, DWORD partCount );
NTSTATUS CciIoUnmount( VOID );

BOOL	 CciIoIsMounted( VOID );
DWORD	 CciIoSectorCount( VOID );			// sectors in the game partition
const CCI_MEDIA_INFO * CciIoMediaInfo( VOID );

// Queues a read IRP whose stack location was prepared by the CdRom0 hook:
// ByteOffset.HighPart = first sector, BufferOffset + ByteOffset.LowPart = destination.
NTSTATUS CciIoQueueRead( PIRP irp );

#endif // __CCIIO_H
