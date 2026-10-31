// Redirects one function import of an already-resident module (for example
// XAM's import of a kernel export) to a replacement, by rewriting the call
// thunk the loader left behind. An export-table patch cannot reach such a
// module because its imports were bound before the patch; this can.
#ifndef __IMPORTHOOK_H
#define __IMPORTHOOK_H

// One installed import hook: the patched variable slot and call thunk, with the
// original words, enough to restore both.
typedef struct _IMPORT_HOOK {
	DWORD ThunkAddr;			// 0 when nothing is installed
	DWORD VariableAddr;
	DWORD SavedVariable;
	DWORD SavedThunk[4];
} IMPORT_HOOK;

// Points targetModule's call of importedModule!ordinal at replacement. Returns
// TRUE and fills hook on success; FALSE if the module, export or import was not
// found (hook->ThunkAddr is left 0, so ImportHookRemove is then a no-op).
BOOL ImportHookInstall( LPCSTR targetModule, LPCSTR importedModule, DWORD ordinal, PVOID replacement, IMPORT_HOOK * hook );

// Restores the variable slot and thunk patched by ImportHookInstall.
VOID ImportHookRemove( IMPORT_HOOK * hook );

#endif // __IMPORTHOOK_H
