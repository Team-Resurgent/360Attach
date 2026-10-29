#include "stdafx.h"
#include "CciStream.h"
#include "FileIo.h"

#include <cci/cci.h>
#include "lz4.h"

// Sectors decoded per pass. The staging buffer holds the compressed blocks of
// one pass, which are never larger than the raw sectors.
#define CCI_CHUNK_SECTORS	32

typedef struct _CCI_PART {
	HANDLE File;
	ULONGLONG IndexOffset;
	DWORD SectorCount;
	DWORD StartSector;		// first logical sector of this part
	BOOL Raw;				// plain sector image (ISO), not a CCI container
} CCI_PART;

static CCI_PART g_Parts[CCIMOUNT_MAX_PARTS];
static DWORD g_PartCount = 0;
static DWORD g_TotalSectors = 0;

// Only the I/O worker thread reads, so these do not need locking.
static DWORD g_IndexBuf[CCI_CHUNK_SECTORS + 1];
static BYTE g_StageBuf[CCI_CHUNK_SECTORS * CCI_SECTOR_SIZE];

static DWORD ReadLE32( const BYTE * p )
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static ULONGLONG ReadLE64( const BYTE * p )
{
	return (ULONGLONG)ReadLE32( p ) | ((ULONGLONG)ReadLE32( p + 4 ) << 32);
}

static NTSTATUS OpenPart( const CHAR * path, CCI_PART * part )
{
	BYTE hdr[CCI_HEADER_SIZE];

	ULONGLONG fileSize = 0;
	part->Raw = FALSE;
	NTSTATUS status = FileOpenRead( path, &part->File, &fileSize );
	if( !NT_SUCCESS(status) )
	{
		LOG( "open %s failed 0x%08X", path, status );
		return status;
	}

	status = FileReadAt( part->File, 0, hdr, sizeof(hdr) );
	if( !NT_SUCCESS(status) )
	{
		LOG( "header read of %s failed 0x%08X", path, status );
		return status;
	}

	if( ReadLE32( hdr ) != CCI_MAGIC )
	{
		// Not a CCI: serve the file as a raw 2048-byte-sector image (an ISO).
		if( fileSize == 0 || (fileSize % CCI_SECTOR_SIZE) != 0 || fileSize / CCI_SECTOR_SIZE >= 0xFFFFFFFFULL )
		{
			LOG( "%s is neither a CCI nor a sector-aligned image", path );
			return STATUS_UNRECOGNIZED_MEDIA;
		}
		part->Raw = TRUE;
		part->IndexOffset = 0;
		part->SectorCount = (DWORD)(fileSize / CCI_SECTOR_SIZE);
		LOG( "part %s: raw image, 0x%X sectors", path, part->SectorCount );
		return STATUS_SUCCESS;
	}

	ULONGLONG uncompressed = ReadLE64( hdr + 8 );
	if( ReadLE32( hdr ) != CCI_MAGIC || ReadLE32( hdr + 4 ) != CCI_HEADER_SIZE ||
		ReadLE32( hdr + 24 ) != CCI_SECTOR_SIZE || hdr[28] != CCI_FORMAT_VERSION ||
		hdr[29] != CCI_INDEX_ALIGNMENT || uncompressed == 0 ||
		(uncompressed % CCI_SECTOR_SIZE) != 0 || (uncompressed / CCI_SECTOR_SIZE) >= 0xFFFFFFFFULL )
	{
		LOG( "%s is not a supported CCI", path );
		return STATUS_UNRECOGNIZED_MEDIA;
	}

	part->IndexOffset = ReadLE64( hdr + 16 );
	part->SectorCount = (DWORD)(uncompressed / CCI_SECTOR_SIZE);
	LOG( "part %s: 0x%X sectors, index at 0x%I64X", path, part->SectorCount, part->IndexOffset );
	return STATUS_SUCCESS;
}

NTSTATUS CciStreamOpen( const CHAR * const * partPaths, DWORD partCount )
{
	CciStreamClose();
	if( partPaths == NULL || partCount == 0 || partCount > CCIMOUNT_MAX_PARTS )
		return STATUS_INVALID_PARAMETER;

	ULONGLONG total = 0;
	for( DWORD i = 0; i < partCount; i++ )
	{
		CCI_PART * part = &g_Parts[i];
		g_PartCount = i + 1; // so CciStreamClose releases what was opened on failure
		NTSTATUS status = OpenPart( partPaths[i], part );
		if( !NT_SUCCESS(status) )
		{
			CciStreamClose();
			return status;
		}
		part->StartSector = (DWORD)total;
		total += part->SectorCount;
		if( total >= 0xFFFFFFFFULL )
		{
			CciStreamClose();
			return STATUS_UNRECOGNIZED_MEDIA;
		}
	}

	g_TotalSectors = (DWORD)total;
	return STATUS_SUCCESS;
}

VOID CciStreamClose( VOID )
{
	for( DWORD i = 0; i < g_PartCount; i++ )
	{
		if( g_Parts[i].File != NULL ) NtClose( g_Parts[i].File );
		g_Parts[i].File = NULL;
	}
	g_PartCount = 0;
	g_TotalSectors = 0;
}

DWORD CciStreamSectorCount( VOID )
{
	return g_TotalSectors;
}

static CCI_PART * FindPart( DWORD lba )
{
	for( DWORD i = g_PartCount; i > 0; i-- )
	{
		if( lba >= g_Parts[i - 1].StartSector ) return &g_Parts[i - 1];
	}
	return NULL;
}

// Decodes up to CCI_CHUNK_SECTORS sectors that all live in one part.
static NTSTATUS ReadChunk( CCI_PART * part, DWORD local, DWORD count, BYTE * dest )
{
	if( part->Raw )
		return FileReadAt( part->File, (ULONGLONG)local * CCI_SECTOR_SIZE, dest, count * CCI_SECTOR_SIZE );

	NTSTATUS status = FileReadAt( part->File, part->IndexOffset + (ULONGLONG)local * 4, g_IndexBuf, (count + 1) * 4 );
	if( !NT_SUCCESS(status) ) return status;

	for( DWORD i = 0; i <= count; i++ )
		g_IndexBuf[i] = ReadLE32( (const BYTE *)&g_IndexBuf[i] );

	ULONGLONG first = (ULONGLONG)(g_IndexBuf[0] & 0x7FFFFFFF) << CCI_INDEX_ALIGNMENT;
	ULONGLONG end = (ULONGLONG)(g_IndexBuf[count] & 0x7FFFFFFF) << CCI_INDEX_ALIGNMENT;
	if( end < first || end - first > sizeof(g_StageBuf) ) return STATUS_FILE_CORRUPT_ERROR;

	status = FileReadAt( part->File, first, g_StageBuf, (DWORD)(end - first) );
	if( !NT_SUCCESS(status) ) return status;

	for( DWORD i = 0; i < count; i++ )
	{
		ULONGLONG pos0 = (ULONGLONG)(g_IndexBuf[i] & 0x7FFFFFFF) << CCI_INDEX_ALIGNMENT;
		ULONGLONG pos1 = (ULONGLONG)(g_IndexBuf[i + 1] & 0x7FFFFFFF) << CCI_INDEX_ALIGNMENT;
		BOOL lz4 = (g_IndexBuf[i] & 0x80000000) != 0;
		if( pos1 < pos0 || pos1 - pos0 > CCI_SECTOR_SIZE ) return STATUS_FILE_CORRUPT_ERROR;

		const BYTE * blk = g_StageBuf + (DWORD)(pos0 - first);
		DWORD span = (DWORD)(pos1 - pos0);
		BYTE * out = dest + i * CCI_SECTOR_SIZE;

		if( !lz4 && span == CCI_SECTOR_SIZE )
		{
			memcpy( out, blk, CCI_SECTOR_SIZE );
			continue;
		}

		// [pad][LZ4 block][pad zero bytes]
		DWORD pad = blk[0];
		if( span < 2 || pad >= span - 1 ) return STATUS_FILE_CORRUPT_ERROR;
		int dec = LZ4_decompress_safe( (const char *)blk + 1, (char *)out, (int)(span - 1 - pad), CCI_SECTOR_SIZE );
		if( dec != (int)CCI_SECTOR_SIZE ) return STATUS_FILE_CORRUPT_ERROR;
	}
	return STATUS_SUCCESS;
}

NTSTATUS CciStreamReadSectors( DWORD lba, DWORD count, BYTE * dest )
{
	if( g_PartCount == 0 ) return STATUS_NO_MEDIA_IN_DEVICE;
	if( lba >= g_TotalSectors || count > g_TotalSectors - lba ) return STATUS_INVALID_PARAMETER;

	while( count > 0 )
	{
		CCI_PART * part = FindPart( lba );
		DWORD local = lba - part->StartSector;
		DWORD n = part->SectorCount - local;
		if( n > count ) n = count;
		if( n > CCI_CHUNK_SECTORS ) n = CCI_CHUNK_SECTORS;

		NTSTATUS status = ReadChunk( part, local, n, dest );
		if( !NT_SUCCESS(status) )
		{
			LOG( "read of sector 0x%X failed 0x%08X", lba, status );
			return status;
		}

		lba += n;
		count -= n;
		dest += n * CCI_SECTOR_SIZE;
	}
	return STATUS_SUCCESS;
}
