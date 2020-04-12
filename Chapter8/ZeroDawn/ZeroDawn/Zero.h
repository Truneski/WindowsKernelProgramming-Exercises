#pragma once
#include "pch.h"

template<typename T>
struct FullItem {
	LIST_ENTRY Entry;
	T Data;
};

struct DirNames {
	UNICODE_STRING name;
};

struct Globals {
	LIST_ENTRY ItemsHead;
	int ItemCount;
	FastMutex Mutex;
};