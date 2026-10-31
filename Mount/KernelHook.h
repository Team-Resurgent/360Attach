// Redirects a kernel export, named by ordinal, to a replacement routine by
// rewriting the entry in the kernel module's export address table. New callers
// that resolve the export (a freshly launched title binding its imports) reach
// the replacement; the kernel image itself is otherwise untouched.
#ifndef __KERNELHOOK_H
#define __KERNELHOOK_H

// Returns the export's original absolute address, to chain to and to pass back
// to KernelUnhookExport, or 0 if the table could not be patched.
DWORD KernelHookExport( DWORD ordinal, PVOID replacement );

// Restores an entry patched by KernelHookExport.
VOID KernelUnhookExport( DWORD ordinal, DWORD original );

// Writes one 32-bit word into a (possibly write-protected) image page: through
// the debug monitor on a devkit, else a direct store. isCode adds the
// instruction-cache maintenance a rewritten code word needs, so hooks that
// patch a call thunk (ImportHook) can share this writer.
VOID HookWrite32( volatile DWORD * dest, DWORD value, BOOL isCode );

#endif // __KERNELHOOK_H
