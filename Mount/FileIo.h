// Kernel file helpers for the image parts. Call from the I/O worker so the
// handles belong to the system process.
#ifndef __FILEIO_H
#define __FILEIO_H

NTSTATUS FileOpenRead( const CHAR * path, HANDLE * file, ULONGLONG * size );
NTSTATUS FileReadAt( HANDLE file, ULONGLONG offset, PVOID buf, DWORD len );

#endif // __FILEIO_H
