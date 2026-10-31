#include "stdafx.h"
#include "ImportHook.h"
#include "KernelHook.h"

// The call thunk the loader writes for an imported function:
//   lis   r11, target@hi      ; +1 when bit 15 of target is set, so the
//   addi  r11, r11, target@lo ; sign-extended addi still forms target
//   mtctr r11
//   bctr
// A caller reaches the function only through this thunk, so rewriting it is
// what moves the call.
static VOID BuildThunk( DWORD destination, DWORD code[4] )
{
	DWORD hi = (destination >> 16) & 0xFFFF;
	if( destination & 0x8000 ) hi++;					// carry for the sign-extended addi
	code[0] = 0x3D600000 | hi;							// lis   r11, hi
	code[1] = 0x396B0000 | (destination & 0xFFFF);		// addi  r11, r11, lo
	code[2] = 0x7D6903A6;								// mtctr r11
	code[3] = 0x4E800420;								// bctr
}

BOOL ImportHookInstall( LPCSTR targetModule, LPCSTR importedModule, DWORD ordinal, PVOID replacement, IMPORT_HOOK * hook )
{
	hook->ThunkAddr = 0;
	if( replacement == NULL ) return FALSE;

	HANDLE target = NULL, imported = NULL;
	PVOID realFn = NULL;
	if( XexGetModuleHandle( (PSTR)targetModule, &target ) != 0 || target == NULL ) return FALSE;
	if( XexGetModuleHandle( (PSTR)importedModule, &imported ) != 0 || imported == NULL ) return FALSE;
	if( XexGetProcedureAddress( imported, ordinal, &realFn ) != 0 || realFn == NULL ) return FALSE;

	PIMAGE_XEX_HEADER header = (PIMAGE_XEX_HEADER)((PLDR_DATA_TABLE_ENTRY)target)->XexHeaderBase;
	if( header == NULL ) return FALSE;
	PXEX_IMPORT_DESCRIPTOR desc = (PXEX_IMPORT_DESCRIPTOR)RtlImageXexHeaderField( header, XEX_HEADER_IMPORTS );
	if( desc == NULL ) return FALSE;

	const BYTE * nameTable = (const BYTE *)(desc + 1);
	PXEX_IMPORT_TABLE table = (PXEX_IMPORT_TABLE)(nameTable + desc->NameTableSize);

	for( DWORD m = 0; m < desc->ModuleCount; m++ )
	{
		if( table->TableSize == 0 ) break;

		// Records pair up as [variable, thunk]: the variable slot holds the
		// loader-resolved function pointer, so a match on it names the ordinal
		// without decoding any code, and the thunk is the record right after.
		for( WORD i = 0; i + 1 < table->ImportCount; i++ )
		{
			DWORD variableAddr = table->ImportStubAddr[i];
			DWORD thunkAddr = table->ImportStubAddr[i + 1];
			if( variableAddr == 0 || thunkAddr == 0 ) continue;
			if( *(volatile DWORD *)variableAddr != (DWORD)realFn ) continue;

			volatile DWORD * thunk = (volatile DWORD *)thunkAddr;
			hook->VariableAddr = variableAddr;
			hook->ThunkAddr = thunkAddr;
			hook->SavedVariable = *(volatile DWORD *)variableAddr;
			for( DWORD w = 0; w < 4; w++ ) hook->SavedThunk[w] = thunk[w];

			HookWrite32( (volatile DWORD *)variableAddr, (DWORD)replacement, FALSE );
			DWORD code[4];
			BuildThunk( (DWORD)replacement, code );
			for( DWORD w = 0; w < 4; w++ ) HookWrite32( &thunk[w], code[w], TRUE );
			return TRUE;
		}

		table = (PXEX_IMPORT_TABLE)((const BYTE *)table + table->TableSize);
	}
	return FALSE;
}

VOID ImportHookRemove( IMPORT_HOOK * hook )
{
	if( hook->ThunkAddr == 0 ) return;

	HookWrite32( (volatile DWORD *)hook->VariableAddr, hook->SavedVariable, FALSE );
	volatile DWORD * thunk = (volatile DWORD *)hook->ThunkAddr;
	for( DWORD w = 0; w < 4; w++ ) HookWrite32( &thunk[w], hook->SavedThunk[w], TRUE );
	hook->ThunkAddr = 0;
}
