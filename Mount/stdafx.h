#pragma once

#include <xtl.h>
#include <stdio.h>

// Kernel and XAM declarations
#include "xkext.h"

#include "CciMountApi.h"

#define LOG( fmt, ... )		DbgPrint( "[CciMount] " fmt "\n", __VA_ARGS__ )

#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE			((NTSTATUS)0xC0000011L)
#endif
#ifndef STATUS_FILE_CORRUPT_ERROR
#define STATUS_FILE_CORRUPT_ERROR	((NTSTATUS)0xC0000102L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND	((NTSTATUS)0xC0000034L)
#endif

// Doubly linked list helpers (the NT inline versions are not in the XDK headers).
static __forceinline VOID InitializeListHead( PLIST_ENTRY head )
{
	head->Flink = head->Blink = head;
}

static __forceinline VOID InsertTailList( PLIST_ENTRY head, PLIST_ENTRY entry )
{
	PLIST_ENTRY blink = head->Blink;
	entry->Flink = head;
	entry->Blink = blink;
	blink->Flink = entry;
	head->Blink = entry;
}

// Returns head itself when the list is empty.
static __forceinline PLIST_ENTRY RemoveHeadList( PLIST_ENTRY head )
{
	PLIST_ENTRY entry = head->Flink;
	PLIST_ENTRY flink = entry->Flink;
	head->Flink = flink;
	flink->Blink = head;
	return entry;
}
