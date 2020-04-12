#include "pch.h"
#include "AutoLock.h"
#include "FastMutex.h"
#include "ZeroCommon.h"
#include "Zero.h"
#include "kstring.h"

#define DRIVER_PREFIX "Zero: "
#define DRIVER_TAG 'oreZ'

// globals
Globals g_Globals;

struct DirectoryEntry {
	UNICODE_STRING DosName;
	UNICODE_STRING NtName;

	void Free() {
		if (DosName.Buffer) {
			ExFreePool(DosName.Buffer);
			DosName.Buffer = nullptr;
		}

		if (NtName.Buffer) {
			ExFreePool(NtName.Buffer);
			NtName.Buffer = nullptr;
		}
	}
};

const int MaxDirectories = 4;
DirectoryEntry DirNames[MaxDirectories];
int DirNamesCount;
FastMutex DirNamesLock;


/*************************************************************************
	Prototypes
*************************************************************************/

int FindDirectory(_In_ PCUNICODE_STRING name, bool dosName);
NTSTATUS ConvertDosNameToNtName(_In_ PCWSTR dosName, _Out_ PUNICODE_STRING ntName);

DRIVER_DISPATCH DelProtectCreateClose, DelProtectDeviceControl;
DRIVER_UNLOAD DelProtectUnloadDriver;

void ClearAll();

void OnProcessNotify(_Inout_ PEPROCESS Process, _In_ HANDLE ProcessId, _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo);

extern "C" NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(RegistryPath);

	// create a standard device object and symbolic link

	PDEVICE_OBJECT DeviceObject = nullptr;
	UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\device\\pathProtect");
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\pathProtect");
	auto symLinkCreated = false;
	bool processCallbacks = false;

	do {
		status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
		if (!NT_SUCCESS(status))
			break;

		status = IoCreateSymbolicLink(&symLink, &devName);
		if (!NT_SUCCESS(status))
			break;

		symLinkCreated = true;

		// Register for Process Notifications
		status = PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, FALSE);
		if (!NT_SUCCESS(status)) {
			KdPrint((DRIVER_PREFIX "failed to register process callback (0x%08X)\n", status));
			break;
		}
		processCallbacks = true;

		DriverObject->DriverUnload = DelProtectUnloadDriver;
		DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = DelProtectCreateClose;
		DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DelProtectDeviceControl;
		DirNamesLock.Init();

	} while (false);

	if (!NT_SUCCESS(status)) {

		if (processCallbacks)
			PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, TRUE);
		if (symLinkCreated)
			IoDeleteSymbolicLink(&symLink);
		if (DeviceObject)
			IoDeleteDevice(DeviceObject);
	}

	return status;
}

NTSTATUS DelProtectCreateClose(PDEVICE_OBJECT, PIRP Irp) {
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS DelProtectDeviceControl(PDEVICE_OBJECT, PIRP Irp) {
	auto stack = IoGetCurrentIrpStackLocation(Irp);
	auto status = STATUS_SUCCESS;

	switch (stack->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_DELPROTECT_ADD_DIR:
	{
		auto name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
		if (!name) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		auto bufferLen = stack->Parameters.DeviceIoControl.InputBufferLength;
		if (bufferLen > 1024) {
			// just too long for a directory
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// make sure there is a NULL terminator somewhere
		name[bufferLen / sizeof(WCHAR) - 1] = L'\0';

		auto dosNameLen = ::wcslen(name);
		if (dosNameLen < 3) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		AutoLock locker(DirNamesLock);
		UNICODE_STRING strName;
		RtlInitUnicodeString(&strName, name);
		if (FindDirectory(&strName, true) >= 0) {
			break;
		}

		if (DirNamesCount == MaxDirectories) {
			status = STATUS_TOO_MANY_NAMES;
			break;
		}

		for (int i = 0; i < MaxDirectories; i++) {
			if (DirNames[i].DosName.Buffer == nullptr) {
				auto len = (dosNameLen + 2) * sizeof(WCHAR);
				auto buffer = (WCHAR*)ExAllocatePoolWithTag(PagedPool, len, DRIVER_TAG);
				if (!buffer) {
					status = STATUS_INSUFFICIENT_RESOURCES;
					break;
				}
				::wcscpy_s(buffer, len / sizeof(WCHAR), name);
				// append a backslash if it's missing
				if (name[dosNameLen - 1] != L'\\')
					::wcscat_s(buffer, dosNameLen + 2, L"\\");

				status = ConvertDosNameToNtName(buffer, &DirNames[i].NtName);
				if (!NT_SUCCESS(status)) {
					ExFreePool(buffer);
					break;
				}

				RtlInitUnicodeString(&DirNames[i].DosName, buffer);
				KdPrint(("Add: %wZ <=> %wZ\n", &DirNames[i].DosName, &DirNames[i].NtName));
				++DirNamesCount;
				break;
			}
		}
		break;
	}

	case IOCTL_DELPROTECT_REMOVE_DIR:
	{
		auto name = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;
		if (!name) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		auto bufferLen = stack->Parameters.DeviceIoControl.InputBufferLength;
		if (bufferLen > 1024) {
			// just too long for a directory
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// make sure there is a NULL terminator somewhere
		name[bufferLen / sizeof(WCHAR) - 1] = L'\0';

		auto dosNameLen = ::wcslen(name);
		if (dosNameLen < 3) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		AutoLock locker(DirNamesLock);
		UNICODE_STRING strName;
		RtlInitUnicodeString(&strName, name);
		int found = FindDirectory(&strName, true);
		if (found >= 0) {
			DirNames[found].Free();
			DirNamesCount--;
		}
		else {
			status = STATUS_NOT_FOUND;
		}
		break;
	}

	case IOCTL_DELPROTECT_CLEAR:
		ClearAll();
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;

}

int FindDirectory(PCUNICODE_STRING name, bool dosName) {
	if (DirNamesCount == 0)
		return -1;

	for (int i = 0; i < MaxDirectories; i++) {
		const auto& dir = dosName ? DirNames[i].DosName : DirNames[i].NtName;
		
		KdPrint(("FindDir - DosName: %ws\n", DirNames[i].DosName.Buffer));

		//if (dir.Buffer && RtlEqualUnicodeString(name, &dir, TRUE))
		if (dir.Buffer && wcsstr(name->Buffer, DirNames[i].DosName.Buffer) != nullptr)
		{
			KdPrint(("DirName Found at index: %d\n", i));
			return i;
		}
	}
	return -1;
}

void ClearAll() {
	AutoLock locker(DirNamesLock);
	for (int i = 0; i < MaxDirectories; i++) {
		if (DirNames[i].DosName.Buffer) {
			ExFreePool(DirNames[i].DosName.Buffer);
			DirNames[i].DosName.Buffer = nullptr;
		}
		if (DirNames[i].NtName.Buffer) {
			ExFreePool(DirNames[i].NtName.Buffer);
			DirNames[i].NtName.Buffer = nullptr;
		}
	}
	DirNamesCount = 0;
}

void DelProtectUnloadDriver(PDRIVER_OBJECT DriverObject) {
	ClearAll();
	PsSetCreateProcessNotifyRoutineEx(OnProcessNotify, TRUE);
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\pathProtect");
	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS ConvertDosNameToNtName(_In_ PCWSTR dosName, _Out_ PUNICODE_STRING ntName) {
	ntName->Buffer = nullptr;
	auto dosNameLen = ::wcslen(dosName);

	if (dosNameLen < 3)
		return STATUS_BUFFER_TOO_SMALL;

	// make sure we have a driver letter
	if (dosName[2] != L'\\' || dosName[1] != L':')
		return STATUS_INVALID_PARAMETER;

	kstring symLink(L"\\??\\", PagedPool, DRIVER_TAG);

	symLink.Append(dosName, 2);		// driver letter and colon

	// prepare to open symbolic link

	UNICODE_STRING symLinkFull;
	symLink.GetUnicodeString(&symLinkFull);
	OBJECT_ATTRIBUTES symLinkAttr;
	InitializeObjectAttributes(&symLinkAttr, &symLinkFull, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	HANDLE hSymLink = nullptr;
	auto status = STATUS_SUCCESS;
	do {
		// open symbolic link
		status = ZwOpenSymbolicLinkObject(&hSymLink, GENERIC_READ, &symLinkAttr);
		if (!NT_SUCCESS(status))
			break;

		USHORT maxLen = 1024;	// arbitrary
		ntName->Buffer = (WCHAR*)ExAllocatePool(PagedPool, maxLen);
		if (!ntName->Buffer) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		ntName->MaximumLength = maxLen;
		// read target of symbolic link
		status = ZwQuerySymbolicLinkObject(hSymLink, ntName, nullptr);
		if (!NT_SUCCESS(status))
			break;
	} while (false);

	if (!NT_SUCCESS(status)) {
		if (ntName->Buffer) {
			ExFreePool(ntName->Buffer);
			ntName->Buffer = nullptr;
		}
	}
	else {
		RtlAppendUnicodeToString(ntName, dosName + 2);	// directory
	}
	if (hSymLink)
		ZwClose(hSymLink);

	return status;
}

void OnProcessNotify(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
	
	UNREFERENCED_PARAMETER(Process);
	UNREFERENCED_PARAMETER(ProcessId);

	// ProcessCreate
	if (CreateInfo) {
		
		if (CreateInfo->FileOpenNameAvailable && CreateInfo->ImageFileName)
		{
			KdPrint(("ImageFilePath: %wZ\n", CreateInfo->ImageFileName));
			AutoLock locker(DirNamesLock);
			if (FindDirectory(CreateInfo->ImageFileName, true) >= 0) {
				
				KdPrint(("File not allowed to Execute: %ws\n", CreateInfo->ImageFileName->Buffer));
				CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
			}
			else {
				KdPrint(("File Allowed to Execute: %ws\n", CreateInfo->ImageFileName->Buffer));
			}
		}
	}
	// ProcessExit
	else {
		
	}
}

