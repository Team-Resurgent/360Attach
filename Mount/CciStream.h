// On-demand CCI reader for the resident driver. libCCI's cci_reader keeps the
// whole sector index in memory (7 MB for a 3.6 GB image, ~15 MB for XGD3),
// which is too much to pin in system memory for the life of a game, so this
// reads only the index entries each request needs. The on-disk format is the
// one in third_party/libCCI/FORMAT.md; split images are stitched per section 7.
#ifndef __CCISTREAM_H
#define __CCISTREAM_H

// Opens every part and validates its header. Must run on a system thread so
// the handles outlive the title that asked for the mount.
NTSTATUS CciStreamOpen( const CHAR * const * partPaths, DWORD partCount );
VOID	 CciStreamClose( VOID );

// Total logical 2048-byte sectors across all parts, 0 when nothing is open.
DWORD	 CciStreamSectorCount( VOID );

// Decodes count sectors starting at logical sector lba into dest.
NTSTATUS CciStreamReadSectors( DWORD lba, DWORD count, BYTE * dest );

#endif // __CCISTREAM_H
