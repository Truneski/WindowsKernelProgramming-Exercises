#pragma once

#include <wdm.h>

class FastMutex {
public:
	void Init();

	void Lock();
	void Unlock();

private:
	FAST_MUTEX _mutex;
};


void FastMutex::Init() {
	ExInitializeFastMutex(&_mutex);
}

void FastMutex::Lock() {
	ExAcquireFastMutex(&_mutex);
}

void FastMutex::Unlock() {
	ExReleaseFastMutex(&_mutex);
}