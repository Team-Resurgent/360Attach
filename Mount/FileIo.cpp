#include "stdafx.h"
#include "FileIo.h"

NTSTATUS FileOpenRead( const CHAR * path, HANDLE * file, ULONGLONG * size )
{
	ANSI_STRING name;
	OBJECT_ATTRIBUTES objectAttr;
	IO_STATUS_BLOCK ioStatus;

	RtlInitAnsiString( &name, path );
	objectAttr.RootDirectory = NULL;
	objectAttr.ObjectName = &name;
	objectAttr.Attributes = OBJ_CASE_INSENSITIVE;

	*file = NULL;
	NTSTATUS status = NtCreateFile( file, GENERIC_READ | SYNCHRONIZE, &objectAttr, &ioStatus, NULL,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE );
	if( !NT_SUCCESS(status) )
	{
		*file = NULL;
		return status;
	}

	if( size != NULL )
	{
		// FileStandardInformation returns STATUS_INVALID_INFO_CLASS on the 360
		FILE_NETWORK_OPEN_INFORMATION info;
		status = NtQueryInformationFile( *file, &ioStatus, &info, sizeof(info), FileNetworkOpenInformation );
		if( !NT_SUCCESS(status) )
		{
			NtClose( *file );
			*file = NULL;
			return status;
		}
		*size = (ULONGLONG)info.EndOfFile.QuadPart;
	}
	return STATUS_SUCCESS;
}

NTSTATUS FileReadAt( HANDLE file, ULONGLONG offset, PVOID buf, DWORD len )
{
	IO_STATUS_BLOCK ioStatus;
	LARGE_INTEGER pos;
	pos.QuadPart = (LONGLONG)offset;

	NTSTATUS status = NtReadFile( file, NULL, NULL, NULL, &ioStatus, buf, len, &pos );
	if( !NT_SUCCESS(status) ) return status;
	if( ioStatus.Information != len ) return STATUS_END_OF_FILE;
	return STATUS_SUCCESS;
}
