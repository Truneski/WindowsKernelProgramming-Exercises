#pragma once

#include "FastMutex.h"

#define DRIVER_TAG 'NICE'
#define DRIVER_PREFIX "RegistryProtector: "

typedef struct _Globals
{
	LIST_ENTRY ItemsHead;
	int ItemCount;
	FastMutex Mutex;
	LARGE_INTEGER RegCookie;

} Globals, *PGlobals;

const int MaxRegKeyCount = 10;

template <typename T>
struct FullItem
{
	LIST_ENTRY Entry;
	T Data;
};