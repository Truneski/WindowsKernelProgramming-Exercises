// ZeroTest.cpp : This file contains the 'main' function. Program execution begins and ends there.

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include "..\ZeroDawn\ZeroCommon.h"

int Error(const char* msg) {
	printf("%s: error=%d\n", msg, ::GetLastError());
	return 1;
}

int PrintUsage() {
	printf("Usage: DelProtectConfig3 <option> [directory]\n");
	printf("\tOption: add, remove or clear\n");
	return 0;
}

int wmain(int argc, const wchar_t* argv[]) {
	if (argc < 2) {
		return PrintUsage();
	}

	// Open the device
	HANDLE hDevice = ::CreateFile(L"\\\\.\\pathProtect", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hDevice == INVALID_HANDLE_VALUE) {
		return Error("failed to open device");
	}


	printf("\nCalling DeviceIoControl METHOD_BUFFERED:\n");
	DWORD returned;
	BOOL success;
	bool badOption = false;

	if (::_wcsicmp(argv[1], L"add") == 0) {
		if (argc < 3)
			return PrintUsage();

		success = ::DeviceIoControl(hDevice, IOCTL_DELPROTECT_ADD_DIR, (PVOID)argv[2], ((DWORD)::wcslen(argv[2]) + 1) * sizeof(WCHAR), nullptr, 0, &returned, nullptr);
	}

	else if (::_wcsicmp(argv[1], L"remove") == 0) {
		if (argc < 3)
			return PrintUsage();

		success = ::DeviceIoControl(hDevice, IOCTL_DELPROTECT_REMOVE_DIR, (PVOID)argv[2], ((DWORD)::wcslen(argv[2]) + 1) * sizeof(WCHAR), nullptr, 0, &returned, nullptr);
	}
	else if (::_wcsicmp(argv[1], L"clear") == 0) {
		success = ::DeviceIoControl(hDevice, IOCTL_DELPROTECT_CLEAR, nullptr, 0, nullptr, 0, &returned, nullptr);
	}
	else {
		badOption = true;
		printf("Unknown option.\n");
	}


	
	if (!badOption) {
		if (!success)
			return Error("Failed in operation");
		else
			printf("Success.\n");
	}

	::CloseHandle(hDevice);

	return 0;
}