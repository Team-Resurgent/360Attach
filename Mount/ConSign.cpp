#include "stdafx.h"
#include "ConSign.h"
#include "CciStream.h"

#include <cci/cci.h>

#define MAX_SIGNED_PACKAGES		64
#define MAX_DIRECTORIES			1024
#define XDVDFS_HEADER_SECTOR	0x20
#define XDVDFS_ATTR_DIRECTORY	0x10

// STFS header: the console signature (certificate + signature) sits at 4 and
// covers the licenses, content ID and header size, 0x22C up to 0x344.
#define STFS_SIGNATURE_OFFSET	0x4
#define STFS_SIGNED_START		0x22C
#define STFS_SIGNED_END			0x344

static DWORD g_SignedSectors[MAX_SIGNED_PACKAGES];
static BYTE * g_SignedData = NULL;		// MAX_SIGNED_PACKAGES sectors
static DWORD g_SignedCount = 0;

static DWORD g_BaseSector = 0;
static DWORD g_SectorCount = 0;
static BYTE g_Sector[CCI_SECTOR_SIZE];

typedef struct _DIRECTORY {
	DWORD Sector;
	DWORD Size;
} DIRECTORY;

static DIRECTORY * g_Directories = NULL;	// MAX_DIRECTORIES pending directories

static WORD ReadLE16( const BYTE * p ) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD ReadLE32( const BYTE * p ) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }

static BOOL ReadSectors( DWORD sector, DWORD count, BYTE * dest )
{
	if( sector >= g_SectorCount || count > g_SectorCount - sector ) return FALSE;
	return NT_SUCCESS(CciStreamReadSectors( g_BaseSector + sector, count, dest ));
}

static BOOL IsSigned( DWORD sector )
{
	for( DWORD i = 0; i < g_SignedCount; i++ )
		if( g_SignedSectors[i] == sector ) return TRUE;
	return FALSE;
}

static VOID SignIfBlankCon( DWORD sector, DWORD size )
{
	if( size < STFS_SIGNED_END || g_SignedCount == MAX_SIGNED_PACKAGES || IsSigned( sector ) ) return;
	if( !ReadSectors( sector, 1, g_Sector ) || memcmp( g_Sector, "CON ", 4 ) != 0 ) return;
	for( DWORD i = STFS_SIGNATURE_OFFSET; i < STFS_SIGNED_START; i++ )
		if( g_Sector[i] != 0 ) return;

	BYTE hash[XECRYPT_SHA_DIGEST_SIZE];
	XeCryptSha( g_Sector + STFS_SIGNED_START, STFS_SIGNED_END - STFS_SIGNED_START, NULL, 0, NULL, 0, hash, sizeof(hash) );
	// Returns 0 on a devkit even though the signature it writes verifies.
	XeKeysConsolePrivateKeySign( hash, (PXE_CONSOLE_SIGNATURE)(g_Sector + STFS_SIGNATURE_OFFSET) );

	g_SignedSectors[g_SignedCount] = sector;
	memcpy( g_SignedData + g_SignedCount * CCI_SECTOR_SIZE, g_Sector, CCI_SECTOR_SIZE );
	g_SignedCount++;
}

// Visits every entry of one directory. Entries form a binary tree whose links
// are offsets in 4-byte units; the walk is bounded so a damaged tree cannot loop.
static VOID ScanDirectory( const BYTE * data, DWORD size, DWORD * pending )
{
	DWORD maxEntries = size / 4;
	WORD * stack = (WORD *)ExAllocatePool( maxEntries * sizeof(WORD) + sizeof(WORD) );
	if( stack == NULL ) return;

	DWORD depth = 0, visited = 0;
	stack[depth++] = 0;
	while( depth > 0 && visited++ < maxEntries )
	{
		DWORD offset = stack[--depth] * 4;
		if( offset + 14 > size ) continue;
		const BYTE * entry = data + offset;
		WORD left = ReadLE16( entry );
		WORD right = ReadLE16( entry + 2 );
		DWORD sector = ReadLE32( entry + 4 );
		DWORD fileSize = ReadLE32( entry + 8 );
		BYTE attributes = entry[12];
		if( left == 0xFFFF || entry[13] == 0 ) continue;

		if( left != 0 && depth < maxEntries ) stack[depth++] = left;
		if( right != 0 && right != 0xFFFF && depth < maxEntries ) stack[depth++] = right;

		if( attributes & XDVDFS_ATTR_DIRECTORY )
		{
			if( fileSize != 0 && *pending < MAX_DIRECTORIES )
			{
				g_Directories[*pending].Sector = sector;
				g_Directories[*pending].Size = fileSize;
				(*pending)++;
			}
		}
		else
		{
			SignIfBlankCon( sector, fileSize );
		}
	}
	ExFreePool( stack );
}

VOID ConSignReset( VOID )
{
	g_SignedCount = 0;
}

VOID ConSignScan( DWORD baseSector, DWORD sectorCount )
{
	ConSignReset();
	g_BaseSector = baseSector;
	g_SectorCount = sectorCount;

	if( g_SignedData == NULL ) g_SignedData = (BYTE *)ExAllocatePool( MAX_SIGNED_PACKAGES * CCI_SECTOR_SIZE );
	if( g_Directories == NULL ) g_Directories = (DIRECTORY *)ExAllocatePool( MAX_DIRECTORIES * sizeof(DIRECTORY) );
	if( g_SignedData == NULL || g_Directories == NULL )
	{
		LOG( "console signing skipped: out of memory" );
		return;
	}

	if( !ReadSectors( XDVDFS_HEADER_SECTOR, 1, g_Sector ) ) return;
	DWORD pending = 0;
	g_Directories[pending].Sector = ReadLE32( g_Sector + 0x14 );
	g_Directories[pending].Size = ReadLE32( g_Sector + 0x18 );
	pending++;

	DWORD scanned = 0;
	while( pending > 0 && scanned++ < MAX_DIRECTORIES )
	{
		DIRECTORY directory = g_Directories[--pending];
		DWORD sectors = (directory.Size + CCI_SECTOR_SIZE - 1) / CCI_SECTOR_SIZE;
		BYTE * data = (BYTE *)ExAllocatePool( sectors * CCI_SECTOR_SIZE );
		if( data == NULL ) continue;
		if( ReadSectors( directory.Sector, sectors, data ) ) ScanDirectory( data, directory.Size, &pending );
		ExFreePool( data );
	}

	if( g_SignedCount != 0 ) LOG( "signed %u CON package(s) with this console's key", g_SignedCount );
}

VOID ConSignApply( DWORD sector, DWORD length, BYTE * dest )
{
	for( DWORD i = 0; i < g_SignedCount; i++ )
	{
		DWORD signedSector = g_SignedSectors[i];
		if( signedSector < sector ) continue;
		DWORD offset = (signedSector - sector) * CCI_SECTOR_SIZE;
		if( offset >= length ) continue;
		DWORD count = length - offset < CCI_SECTOR_SIZE ? length - offset : CCI_SECTOR_SIZE;
		memcpy( dest + offset, g_SignedData + i * CCI_SECTOR_SIZE, count );
	}
}
