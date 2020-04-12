#include "pch.h"

// Headers
#include "AutoLock.h"
#include "RegistryProtector.h"
#include "RegistryProtectorCommon.h"

// PROTOTYPES
DRIVER_UNLOAD DriverUnload;
DRIVER_DISPATCH DriverCreateClose, DriverDeviceControl;
void PushItem(LIST_ENTRY* entry);

NTSTATUS OnRegistryNotify(PVOID context, PVOID arg1, PVOID arg2);

// Globals

Globals g_Globals;

extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING)
{
	KdPrint((DRIVER_PREFIX "Entering the Dark of the Moon!\n"));

	auto status = STATUS_SUCCESS;
	UNICODE_STRING deviceName = RTL_CONSTANT_STRING(L"\\Device\\" DEVICE_NAME);
	UNICODE_STRING symName = RTL_CONSTANT_STRING(L"\\??\\" DEVICE_NAME);
	PDEVICE_OBJECT DeviceObject = nullptr;
	bool symlinkCreated = false;

	// g_Globals.Init();
	// Initialize linked list head
	InitializeListHead(&g_Globals.ItemsHead);
	// Initialize Item counter
	g_Globals.ItemCount = 0;
	// Initialize Fastmutex
	g_Globals.Mutex.Init();

	do
	{
		// Create the device
		status = IoCreateDevice(DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN, 0, TRUE, &DeviceObject);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX "failed to create device (0x%08X)\n", status));
			break;
		}

		status = IoCreateSymbolicLink(&symName, &deviceName);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX "failed to create sym link (0x%08X)\n", status));
			break;
		}
		symlinkCreated = true;

		UNICODE_STRING altitude = RTL_CONSTANT_STRING(L"7657.124");
		status = CmRegisterCallbackEx(OnRegistryNotify, &altitude, DriverObject, nullptr, &g_Globals.RegCookie, nullptr);
		if (!NT_SUCCESS(status)) {
			KdPrint((DRIVER_PREFIX "failed to set registry callback (status=%08X)\n", status));
			break;
		}

	} while (false);

	if (!NT_SUCCESS(status))
	{
		if (symlinkCreated)
			IoDeleteSymbolicLink(&symName);
		if (DeviceObject)
			IoDeleteDevice(DeviceObject);
	}

	DriverObject->DriverUnload = DriverUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDeviceControl;

	KdPrint((DRIVER_PREFIX "As we leave hell, we go back to the light of unloading.\n"));

	return status;
}


// Good
void DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	// unregister callbacks
	auto status = CmUnRegisterCallback(g_Globals.RegCookie);
	if (!NT_SUCCESS(status)) {
		KdPrint(("failed on CmUnRegisterCallback (0x%08X)\n", status));
	}

	UNICODE_STRING symName = RTL_CONSTANT_STRING(L"\\??\\" DEVICE_NAME);
	IoDeleteSymbolicLink(&symName);
	IoDeleteDevice(DriverObject->DeviceObject);

	while (!IsListEmpty(&g_Globals.ItemsHead))
	{
		auto entry = RemoveHeadList(&g_Globals.ItemsHead);
		ExFreePool(CONTAINING_RECORD(entry, FullItem<RegKeyProtectInfo>, Entry));
	}
	g_Globals.ItemCount = 0;

	return;
}

// Good
NTSTATUS DriverCreateClose(_In_ PDEVICE_OBJECT, _In_ PIRP Irp)
{
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

// Good
void PushItem(LIST_ENTRY* entry)
{
	AutoLock<FastMutex> lock(g_Globals.Mutex); // till now to the end of the function we will have Mutex
											   // and will be freed on destructor at the end of the function
	if (g_Globals.ItemCount > MaxRegKeyCount)
	{
		// too many items, remove oldest one
		auto head = RemoveHeadList(&g_Globals.ItemsHead);
		g_Globals.ItemCount--;
		ExFreePool(CONTAINING_RECORD(head, FullItem<RegKeyProtectInfo*>, Entry));
	}

	InsertTailList(&g_Globals.ItemsHead, entry);
	g_Globals.ItemCount++;
}

// TBD
NTSTATUS DriverDeviceControl(_In_ PDEVICE_OBJECT, _In_ PIRP Irp)
{
	auto IrpStack = IoGetCurrentIrpStackLocation(Irp);
	auto status = STATUS_SUCCESS;
	auto len = 0;

	switch (IrpStack->Parameters.DeviceIoControl.IoControlCode)
	{
	case IOCTL_REGKEY_PROTECT_ADD:
	{
		auto inputBufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
		auto inputBuffer = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;

		if (inputBufferSize == 0 || inputBuffer == nullptr)
		{
			KdPrint((DRIVER_PREFIX "The Registry Key passed is not Correct.\n"));
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		KdPrint(("The Registry Path to Protect is: %ws", inputBuffer));

		auto size = sizeof(FullItem<RegKeyProtectInfo>);
		auto info = (FullItem<RegKeyProtectInfo>*)ExAllocatePoolWithTag(PagedPool, size, DRIVER_TAG);
		if (info == nullptr)
		{
			KdPrint((DRIVER_PREFIX "Failed to Allocate Memory.\n"));
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		RtlZeroMemory(info, size);

		auto& item = info->Data;
		//auto RegKeyLength = inputBufferSize / sizeof(WCHAR);
		RtlCopyMemory(item.KeyName, inputBuffer, inputBufferSize);
		PushItem(&info->Entry);
		break;
	}


	case IOCTL_REGKEY_PROTECT_REMOVE:
	{
		auto inputBufferSize = IrpStack->Parameters.DeviceIoControl.InputBufferLength;
		auto inputBuffer = (WCHAR*)Irp->AssociatedIrp.SystemBuffer;

		if (inputBufferSize == 0 || inputBuffer == nullptr)
		{
			KdPrint((DRIVER_PREFIX "The Registry Key passed is not Correct.\n"));
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		KdPrint(("The Registry Path to Protect is: %ws", inputBuffer));

		AutoLock<FastMutex> lock(g_Globals.Mutex);

		UNICODE_STRING inputBF;
		RtlInitUnicodeString(&inputBF, inputBuffer);

		for (auto i = 0; i < g_Globals.ItemCount; i++)
		{
			auto entry = RemoveHeadList(&g_Globals.ItemsHead);
			auto info = CONTAINING_RECORD(entry, FullItem<RegKeyProtectInfo*>, Entry);
			auto kName = (WCHAR*)&info->Data;

			UNICODE_STRING tbcName;
			RtlInitUnicodeString(&tbcName, kName);

			if (RtlCompareUnicodeString(&inputBF, &tbcName, TRUE) == 0)
			{
				KdPrint(("Found a Matching Protected key. Removing it from the LinkedList."));
				g_Globals.ItemCount--;
				ExFreePool(CONTAINING_RECORD(entry, FullItem<RegKeyProtectInfo>, Entry));
				break;
			}
			InsertTailList(&g_Globals.ItemsHead, entry);
		}
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	case IOCTL_REGKEY_PROTECT_CLEAR:
	{
		KdPrint(("Sounding The Purge Siren! Removing all Protected RegKeys.\n"));
		while (!IsListEmpty(&g_Globals.ItemsHead))
		{
			auto entry = RemoveHeadList(&g_Globals.ItemsHead);
			ExFreePool(CONTAINING_RECORD(entry, FullItem<RegKeyProtectInfo>, Entry));
		}
		g_Globals.ItemCount = 0;

		break;
	}
		
	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;

	}
	// complete the request
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = len;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

// TBD
NTSTATUS OnRegistryNotify(PVOID, PVOID arg1, PVOID arg2) {

	auto status = STATUS_SUCCESS;

	switch ((REG_NOTIFY_CLASS)(ULONG_PTR)arg1) {
	case RegNtPreSetValueKey: 
	{
		auto preInfo = static_cast<PREG_SET_VALUE_KEY_INFORMATION>(arg2);
		PCUNICODE_STRING keyName = nullptr;
		if (!NT_SUCCESS(CmCallbackGetKeyObjectID(&g_Globals.RegCookie, preInfo->Object, nullptr, &keyName))) {
			break;
		}
		
		// KdPrint(("Keyname to be Compared is: %wZ", keyName));

		AutoLock<FastMutex> lock(g_Globals.Mutex);

		for (auto i = 0; i < g_Globals.ItemCount; i++)
		{
			auto entry = RemoveHeadList(&g_Globals.ItemsHead);
			auto info = CONTAINING_RECORD(entry, FullItem<RegKeyProtectInfo*>, Entry);
			auto kName = (WCHAR*)&info->Data;

			UNICODE_STRING tbcName;
			RtlInitUnicodeString(&tbcName, kName);
			// KdPrint(("KeyName(U_C) In Linked List is: %wZ!\n", tbcName));

			if (RtlCompareUnicodeString(keyName, &tbcName, TRUE) == 0)
			{
				KdPrint(("Found a Matching Protected key. Blocking Any Modification Attempts."));
				InsertTailList(&g_Globals.ItemsHead, entry);
				status = STATUS_CALLBACK_BYPASS;
				break;
			}
			InsertTailList(&g_Globals.ItemsHead, entry);
			
		}
	}
	}

	return status;
}