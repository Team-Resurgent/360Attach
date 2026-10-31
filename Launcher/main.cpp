// Launcher: put default.xex and mount.xex in a folder next to a .cci (or a
// split set name.1.cci, name.2.cci, ...). Launching it loads CciMount as a
// resident system DLL, mounts the image as the DVD drive, and starts the
// disc's default.xex.
#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include "xkext.h"

#include <cci/cci.h>
#include "CciMountApi.h"

#define LOG_PATH		"game:\\Launcher.log"
#define DISC_EXEC		"\\Device\\Cdrom0\\default.xex"
#define DISC_ROOT		"\\Device\\Cdrom0"

static FILE * g_Log = NULL;

static VOID Log( const CHAR * fmt, ... )
{
	CHAR line[512];
	va_list args;
	va_start( args, fmt );
	_vsnprintf_s( line, sizeof(line), _TRUNCATE, fmt, args );
	va_end( args );

	DbgPrint( "[Launcher] %s\n", line );
	if( g_Log != NULL )
	{
		fprintf( g_Log, "%s\r\n", line );
		fflush( g_Log );
	}
}

// ---------------------------------------------------------------------------
// Finding the image

typedef struct _IMAGE_SET {
	CHAR Names[CCIMOUNT_MAX_PARTS][MAX_PATH];	// file names inside game:
	DWORD Count;
} IMAGE_SET;

static BOOL EndsWithExt( const CHAR * name, const CHAR * ext )
{
	size_t len = strlen( name );
	size_t extLen = strlen( ext );
	return len > extLen && _stricmp( name + len - extLen, ext ) == 0;
}

static BOOL FileExists( const CHAR * name )
{
	CHAR path[MAX_PATH];
	sprintf_s( path, sizeof(path), "game:\\%s", name );
	return GetFileAttributes( path ) != (DWORD)-1;	// INVALID_FILE_ATTRIBUTES, absent from the XDK headers
}

// name.cci is a single part; name.N.cci belongs to the set name.1.cci, name.2.cci, ...
static BOOL BuildImageSet( const CHAR * name, IMAGE_SET * set )
{
	CHAR stem[MAX_PATH];
	strcpy_s( stem, sizeof(stem), name );
	stem[strlen( stem ) - 4] = '\0';

	CHAR * dot = strrchr( stem, '.' );
	BOOL numbered = dot != NULL && dot[1] != '\0';
	for( CHAR * p = dot ? dot + 1 : NULL; numbered && *p; p++ )
		if( *p < '0' || *p > '9' ) numbered = FALSE;

	set->Count = 0;
	if( !numbered )
	{
		strcpy_s( set->Names[0], MAX_PATH, name );
		set->Count = 1;
		return TRUE;
	}

	// Parts keep the image extension (name.1.cci or name.1.iso); they are read in part-number order.
	CHAR ext[8];
	strcpy_s( ext, sizeof(ext), name + strlen( name ) - 4 );

	*dot = '\0';
	for( DWORD i = 1; i <= CCIMOUNT_MAX_PARTS; i++ )
	{
		CHAR part[MAX_PATH];
		sprintf_s( part, sizeof(part), "%s.%u%s", stem, i, ext );
		if( !FileExists( part ) ) break;
		strcpy_s( set->Names[set->Count++], MAX_PATH, part );
	}
	if( set->Count == 0 ) Log( "split set %s has no part 1", name );
	return set->Count > 0;
}

// Looks for the first image with the given extension, preferring nothing else.
static BOOL FindImageWithExt( const CHAR * pattern, const CHAR * ext, IMAGE_SET * set )
{
	WIN32_FIND_DATA fd;
	HANDLE find = FindFirstFile( pattern, &fd );
	if( find == INVALID_HANDLE_VALUE ) return FALSE;

	BOOL found = FALSE;
	do
	{
		if( (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && EndsWithExt( fd.cFileName, ext ) )
			found = BuildImageSet( fd.cFileName, set );
	} while( !found && FindNextFile( find, &fd ) );
	FindClose( find );
	return found;
}

// A .cci wins over a .iso when both are present. The search lists the whole
// folder and filters by extension in code: FATX's FindFirstFile wildcard does
// not match a split part like name.1.iso with a "*.iso" pattern (the '*' does
// not span the extra dot), so "game:\*" is used and EndsWithExt does the work.
static BOOL FindImage( IMAGE_SET * set )
{
	if( FindImageWithExt( "game:\\*", ".cci", set ) ) return TRUE;
	if( FindImageWithExt( "game:\\*", ".iso", set ) ) return TRUE;
	Log( "no .cci or .iso file next to default.xex; files in this folder:" );
	WIN32_FIND_DATA all;
	HANDLE list = FindFirstFile( "game:\\*", &all );
	if( list != INVALID_HANDLE_VALUE )
	{
		do Log( "  %s%s", all.cFileName, (all.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/" : "" );
		while( FindNextFile( list, &all ) );
		FindClose( list );
	}
	return FALSE;
}

// ---------------------------------------------------------------------------
// Validating it with libCCI

static int64_t Win32Read( void * ctx, uint64_t offset, void * buf, size_t len )
{
	HANDLE file = (HANDLE)ctx;
	LONG high = (LONG)(offset >> 32);
	if( SetFilePointer( file, (LONG)(offset & 0xFFFFFFFF), &high, FILE_BEGIN ) == INVALID_SET_FILE_POINTER &&
		GetLastError() != NO_ERROR )
		return CCI_ERR_IO;

	DWORD got = 0;
	if( !ReadFile( file, buf, (DWORD)len, &got, NULL ) || got != len ) return CCI_ERR_IO;
	return (int64_t)len;
}

static void Win32Close( void * ctx )
{
	CloseHandle( (HANDLE)ctx );
}

// Offsets where the XDVDFS game partition can begin: after the video partition
// in a full XGD2 or XGD3 dump, or at 0 in a game-partition-only image.
static const uint64_t partitionStarts[] = { 0xFD90000ULL, 0x2080000ULL, 0x0ULL };

// A raw ISO, or a split ISO set, is read by sector straight from its files. The
// parts are concatenated in order, as the driver does.
typedef struct _ISO_PARTS {
	HANDLE File[CCIMOUNT_MAX_PARTS];
	ULONGLONG Start[CCIMOUNT_MAX_PARTS];
	ULONGLONG Size[CCIMOUNT_MAX_PARTS];
	DWORD Count;
} ISO_PARTS;

static VOID CloseIsoParts( ISO_PARTS * iso )
{
	for( DWORD i = 0; i < iso->Count; i++ )
		if( iso->File[i] != NULL ) CloseHandle( iso->File[i] );
	memset( iso, 0, sizeof(*iso) );
}

static BOOL OpenIsoParts( const IMAGE_SET * set, ISO_PARTS * iso )
{
	memset( iso, 0, sizeof(*iso) );
	iso->Count = set->Count;
	ULONGLONG start = 0;
	for( DWORD i = 0; i < set->Count; i++ )
	{
		CHAR path[MAX_PATH];
		sprintf_s( path, sizeof(path), "game:\\%s", set->Names[i] );
		iso->File[i] = CreateFile( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		if( iso->File[i] == INVALID_HANDLE_VALUE )
		{
			Log( "cannot open %s (%u)", path, GetLastError() );
			iso->File[i] = NULL;
			CloseIsoParts( iso );
			return FALSE;
		}
		iso->Start[i] = start;
		iso->Size[i] = GetFileSize( iso->File[i], NULL );
		start += iso->Size[i];
	}
	return TRUE;
}

// Reads len bytes at an absolute offset in the concatenated image, across parts.
static BOOL ReadIsoAt( const ISO_PARTS * iso, ULONGLONG offset, void * buf, DWORD len )
{
	BYTE * dest = (BYTE *)buf;
	while( len > 0 )
	{
		DWORD i = 0;
		while( i < iso->Count && !(offset >= iso->Start[i] && offset < iso->Start[i] + iso->Size[i]) ) i++;
		if( i == iso->Count ) return FALSE;

		DWORD n = (DWORD)(iso->Start[i] + iso->Size[i] - offset);
		if( n > len ) n = len;
		if( Win32Read( iso->File[i], offset - iso->Start[i], dest, n ) != (int64_t)n ) return FALSE;

		offset += n;
		dest += n;
		len -= n;
	}
	return TRUE;
}

static BOOL ValidateIso( const IMAGE_SET * set )
{
	ISO_PARTS iso;
	if( !OpenIsoParts( set, &iso ) ) return FALSE;

	BOOL hasGame = FALSE;
	static const char magic[] = "MICROSOFT*XBOX*MEDIA";
	for( int i = 0; i < ARRAYSIZE(partitionStarts) && !hasGame; i++ )
	{
		char header[sizeof(magic) - 1];
		if( ReadIsoAt( &iso, partitionStarts[i] + 0x10000, header, sizeof(header) ) &&
			memcmp( header, magic, sizeof(header) ) == 0 )
		{
			Log( "ISO game partition at 0x%I64X", partitionStarts[i] );
			hasGame = TRUE;
		}
	}
	if( !hasGame ) Log( "ISO has no Xbox 360 game partition" );
	CloseIsoParts( &iso );
	return hasGame;
}

// Opens the set as one volume and checks it holds an XDVDFS game partition,
// so a bad image fails here with a message instead of as a dead disc.
static BOOL ValidateImage( const IMAGE_SET * set )
{
	if( EndsWithExt( set->Names[0], ".iso" ) ) return ValidateIso( set );

	cci_reader * parts[CCIMOUNT_MAX_PARTS];
	DWORD opened = 0;
	BOOL ok = TRUE;

	for( DWORD i = 0; i < set->Count && ok; i++ )
	{
		CHAR path[MAX_PATH];
		sprintf_s( path, sizeof(path), "game:\\%s", set->Names[i] );
		HANDLE file = CreateFile( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		if( file == INVALID_HANDLE_VALUE )
		{
			Log( "cannot open %s (%u)", path, GetLastError() );
			ok = FALSE;
			break;
		}
		int ret = cci_reader_open( &parts[opened], Win32Read, file, Win32Close );
		if( ret != CCI_OK )
		{
			Log( "%s: %s", set->Names[i], cci_strerror( ret ) );
			ok = FALSE;
			break;
		}
		Log( "part %u: %s, %I64u sectors", i + 1, set->Names[i], cci_reader_sector_count( parts[opened] ) );
		opened++;
	}

	cci_volume * volume = NULL;
	if( ok && cci_volume_open( &volume, parts, opened ) != CCI_OK ) ok = FALSE;
	if( !ok )
	{
		for( DWORD i = 0; i < opened; i++ ) cci_reader_free( parts[i] );
		return FALSE;
	}

	static const char magic[] = "MICROSOFT*XBOX*MEDIA";
	BOOL hasGame = FALSE;
	for( int i = 0; i < ARRAYSIZE(partitionStarts) && !hasGame; i++ )
	{
		char header[sizeof(magic) - 1];
		if( cci_volume_read( volume, partitionStarts[i] + 0x10000, header, sizeof(header) ) == CCI_OK &&
			memcmp( header, magic, sizeof(header) ) == 0 )
		{
			Log( "game partition at 0x%I64X, image is 0x%I64X bytes", partitionStarts[i], cci_volume_size( volume ) );
			hasGame = TRUE;
		}
	}
	if( !hasGame ) Log( "image has no Xbox 360 game partition" );

	cci_volume_free( volume );
	return hasGame;
}

// ---------------------------------------------------------------------------
// Loading CciMount and launching

// Directory of this title as a kernel path, e.g. \Device\Harddisk0\Partition1\Games\Foo
static BOOL GetTitleDirectory( CHAR * out, DWORD outSize )
{
	PLDR_DATA_TABLE_ENTRY entry = *XexExecutableModuleHandle;
	if( entry == NULL || entry->FullDllName.Buffer == NULL ) return FALSE;

	DWORD len = entry->FullDllName.Length / sizeof(WCHAR);
	if( len >= outSize ) return FALSE;
	for( DWORD i = 0; i < len; i++ ) out[i] = (CHAR)entry->FullDllName.Buffer[i];
	out[len] = '\0';

	CHAR * slash = strrchr( out, '\\' );
	if( slash == NULL ) return FALSE;
	*slash = '\0';
	return TRUE;
}

typedef struct _LOAD_REQUEST {
	const CHAR * Path;
	HANDLE Module;
	NTSTATUS Status;
	volatile BOOL Done;
} LOAD_REQUEST;

static DWORD WINAPI LoadOnSystemThread( LPVOID param )
{
	LOAD_REQUEST * req = (LOAD_REQUEST *)param;
	DbgPrint( "[Launcher] system thread running\n" );
	req->Status = XexLoadImage( req->Path, XEX_LOADIMG_TYPE_SYSTEM_DLL, 0, &req->Module );
	DbgPrint( "[Launcher] XexLoadImage returned 0x%08X\n", req->Status );
	__lwsync();
	req->Done = TRUE;
	return 0;
}

// Generated by tools/embed_cci_mount.py from the mount.xex build output.
extern "C" {
extern const BYTE g_CciMountImage[];
extern const DWORD g_CciMountImageSize;
}

// Writes the embedded mount.xex to this title's folder. Skipped when a file
// of the same size is already there.
static BOOL SameAsEmbedded( HANDLE file )
{
	BYTE chunk[4096];
	DWORD offset = 0;
	while( offset < g_CciMountImageSize )
	{
		DWORD want = g_CciMountImageSize - offset;
		if( want > sizeof(chunk) ) want = sizeof(chunk);
		DWORD got = 0;
		if( !ReadFile( file, chunk, want, &got, NULL ) || got != want ) return FALSE;
		if( memcmp( chunk, g_CciMountImage + offset, want ) != 0 ) return FALSE;
		offset += want;
	}
	// the file must end where the embedded image does
	DWORD extra = 0;
	return ReadFile( file, chunk, 1, &extra, NULL ) && extra == 0;
}

// Writes the embedded mount.xex to this title's folder unless the file there is
// already byte-for-byte the same, so a rebuilt driver replaces an older copy.
static BOOL WriteEmbeddedDriver( VOID )
{
	HANDLE file = CreateFile( "game:\\" CCIMOUNT_MODULE_NAME, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
	if( file != INVALID_HANDLE_VALUE )
	{
		BOOL same = SameAsEmbedded( file );
		CloseHandle( file );
		if( same ) return TRUE;
		Log( "%s is out of date, rewriting it", CCIMOUNT_MODULE_NAME );
	}

	file = CreateFile( "game:\\" CCIMOUNT_MODULE_NAME, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if( file == INVALID_HANDLE_VALUE )
	{
		Log( "cannot create %s (%u)", CCIMOUNT_MODULE_NAME, GetLastError() );
		return FALSE;
	}
	DWORD written = 0;
	BOOL ok = WriteFile( file, g_CciMountImage, g_CciMountImageSize, &written, NULL ) && written == g_CciMountImageSize;
	CloseHandle( file );
	if( !ok ) Log( "failed to write %s", CCIMOUNT_MODULE_NAME );
	return ok;
}

// Loaded from a system thread so the module belongs to the system process and
// stays resident after this title is replaced by the game.
static HANDLE LoadCciMount( const CHAR * titleDir )
{
	HANDLE module = NULL;
	if( XexGetModuleHandle( CCIMOUNT_MODULE_NAME, &module ) == 0 && module != NULL )
	{
		Log( "%s already resident", CCIMOUNT_MODULE_NAME );
		return module;
	}

	// The driver ships inside this image; write it next to us before loading.
	if( !WriteEmbeddedDriver() ) return NULL;

	CHAR path[MAX_PATH];
	sprintf_s( path, sizeof(path), "%s\\%s", titleDir, CCIMOUNT_MODULE_NAME );
	LOAD_REQUEST req = { path, NULL, STATUS_UNSUCCESSFUL, FALSE };

	// XapiThreadStartup runs the routine with the title's XAPI thread setup.
	// The handle belongs to the system process, so poll for completion rather
	// than waiting on it from this title thread.
	HANDLE thread = NULL;
	DWORD threadId = 0;
	NTSTATUS status = ExCreateThread( &thread, 0x10000, &threadId, (PVOID)XapiThreadStartup, LoadOnSystemThread, &req, EX_CREATE_FLAG_SYSTEM );
	if( !NT_SUCCESS(status) )
	{
		Log( "system thread creation failed 0x%08X", status );
		return NULL;
	}
	// Without the processor pin and priority boost the thread never ran on the
	// devkit; ResumeThread is a no-op safeguard (it reported a count of 0).
	XSetThreadProcessor( thread, 3 );
	SetThreadPriority( thread, THREAD_PRIORITY_TIME_CRITICAL );
	DWORD suspendCount = ResumeThread( thread );
	Log( "loading %s (thread %u, suspend count %d)", path, threadId, (int)suspendCount );
	for( DWORD waited = 0; !req.Done; waited += 10 )
	{
		if( waited >= 10000 )
		{
			Log( "timed out loading %s", path );
			return NULL;
		}
		Sleep( 10 );
	}

	if( !NT_SUCCESS(req.Status) )
	{
		Log( "XexLoadImage %s failed 0x%08X", path, req.Status );
		return NULL;
	}
	return req.Module;
}

template<typename T> static BOOL Resolve( HANDLE module, DWORD ordinal, T * fn )
{
	*fn = NULL;
	return XexGetProcedureAddress( module, ordinal, fn ) == 0 && *fn != NULL;
}

static BOOL MountAndLaunch( VOID )
{
	IMAGE_SET set;
	if( !FindImage( &set ) || !ValidateImage( &set ) ) return FALSE;

	CHAR titleDir[MAX_PATH];
	if( !GetTitleDirectory( titleDir, sizeof(titleDir) ) )
	{
		Log( "cannot determine the title's device path" );
		return FALSE;
	}
	Log( "title directory %s", titleDir );

	HANDLE module = LoadCciMount( titleDir );
	if( module == NULL ) return FALSE;

	PFN_CCIGETVERSION getVersion;
	PFN_CCIMOUNT mount;
	if( !Resolve( module, CCIMOUNT_ORD_GETVERSION, &getVersion ) || !Resolve( module, CCIMOUNT_ORD_MOUNT, &mount ) )
	{
		Log( "%s is missing exports", CCIMOUNT_MODULE_NAME );
		return FALSE;
	}
	if( getVersion() != CCIMOUNT_VERSION )
	{
		// an older copy is still resident from an earlier launch; reboot to replace it
		Log( "resident %s is version 0x%08X, expected 0x%08X", CCIMOUNT_MODULE_NAME, getVersion(), CCIMOUNT_VERSION );
		return FALSE;
	}

	CHAR paths[CCIMOUNT_MAX_PARTS][MAX_PATH];
	const CHAR * pathPtrs[CCIMOUNT_MAX_PARTS];
	for( DWORD i = 0; i < set.Count; i++ )
	{
		sprintf_s( paths[i], MAX_PATH, "%s\\%s", titleDir, set.Names[i] );
		pathPtrs[i] = paths[i];
	}

	NTSTATUS status = mount( pathPtrs, set.Count );
	if( !NT_SUCCESS(status) )
	{
		Log( "mount failed 0x%08X", status );
		return FALSE;
	}

	// Record the media ID the mount serves (through the hooked kernel export), so
	// Launcher.log shows what identity a disc title will see.
	{
		HANDLE kernel = NULL;
		PVOID fn = NULL;
		if( XexGetModuleHandle( MODULE_KERNEL, &kernel ) == 0 && kernel != NULL &&
			XexGetProcedureAddress( kernel, 690, &fn ) == 0 && fn != NULL )
		{
			BYTE id[0x10];
			memset( id, 0, sizeof(id) );
			((NTSTATUS (NTAPI *)(PBYTE, BOOL))fn)( id, FALSE );
			Log( "disc media ID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
				id[0],id[1],id[2],id[3],id[4],id[5],id[6],id[7],id[8],id[9],id[10],id[11],id[12],id[13],id[14],id[15] );
		}
	}

	Log( "mounted, launching %s", DISC_EXEC );
	if( g_Log != NULL ) { fclose( g_Log ); g_Log = NULL; }

	// Give XAM a moment to finish re-probing the drive after the tray cycle.
	Sleep( 1000 );
	XamLoaderLaunchTitleEx( DISC_EXEC, DISC_ROOT, NULL, 0 );
	return TRUE;
}

VOID __cdecl main()
{
	fopen_s( &g_Log, LOG_PATH, "w" );
	Log( "Launcher starting" );

	if( !MountAndLaunch() )
	{
		Log( "giving up, returning to the dashboard" );
		if( g_Log != NULL ) fclose( g_Log );
		XLaunchNewImage( XLAUNCH_KEYWORD_DEFAULT_APP, 0 );
	}

	for( ;; ) Sleep( 1000 );
}
