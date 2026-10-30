// Console signing: a converted disc (Xbox360Convert) carries its rebuilt STFS
// packages as CON packages with a blank certificate and signature. At mount
// they are signed with this console's key and the signed first sectors are
// served in place of the blank ones, so the console accepts the packages.
#ifndef __CONSIGN_H
#define __CONSIGN_H

// Walks the game partition and signs every blank CON package. Runs on the
// worker thread during mount, before any read is served.
VOID ConSignScan( DWORD baseSector, DWORD sectorCount );
VOID ConSignReset( VOID );

// Lays the signed sectors over a read of length bytes from partition sector.
VOID ConSignApply( DWORD sector, DWORD length, BYTE * dest );

#endif // __CONSIGN_H
