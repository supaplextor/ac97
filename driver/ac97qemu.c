#include <ntddk.h>

typedef struct _AC97_DEVICE_EXTENSION {
    PDEVICE_OBJECT LowerDevice;
} AC97_DEVICE_EXTENSION, *PAC97_DEVICE_EXTENSION;

DRIVER_UNLOAD Ac97Unload;
DRIVER_ADD_DEVICE Ac97AddDevice;
_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH Ac97DispatchPnp;
IO_COMPLETION_ROUTINE Ac97CompletionRoutine;

NTSTATUS
Ac97CompletionRoutine(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
Ac97ForwardAndWait(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    KEVENT event;
    NTSTATUS status;
    PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, Ac97CompletionRoutine, &event, TRUE, TRUE, TRUE);

    status = IoCallDriver(extension->LowerDevice, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;
}

NTSTATUS
Ac97DispatchPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(extension->LowerDevice, Irp);
}

NTSTATUS
Ac97DispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (irpStack->MinorFunction) {
    case IRP_MN_START_DEVICE: {
        NTSTATUS status = Ac97ForwardAndWait(DeviceObject, Irp);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    case IRP_MN_REMOVE_DEVICE: {
        PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
        NTSTATUS status;

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(extension->LowerDevice, Irp);
        IoDetachDevice(extension->LowerDevice);
        IoDeleteDevice(DeviceObject);
        return status;
    }
    default:
        return Ac97DispatchPassThrough(DeviceObject, Irp);
    }
}

NTSTATUS
Ac97AddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    PAC97_DEVICE_EXTENSION extension;

    status = IoCreateDevice(
        DriverObject,
        sizeof(AC97_DEVICE_EXTENSION),
        NULL,
        FILE_DEVICE_SOUND,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    extension = (PAC97_DEVICE_EXTENSION)deviceObject->DeviceExtension;
    extension->LowerDevice = IoAttachDeviceToDeviceStack(deviceObject, PhysicalDeviceObject);
    if (extension->LowerDevice == NULL) {
        IoDeleteDevice(deviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    deviceObject->Flags |= DO_POWER_PAGABLE;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

VOID
Ac97Unload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    ULONG majorFunction;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = Ac97Unload;
    DriverObject->DriverExtension->AddDevice = Ac97AddDevice;

    for (majorFunction = 0; majorFunction <= IRP_MJ_MAXIMUM_FUNCTION; majorFunction++) {
        DriverObject->MajorFunction[majorFunction] = Ac97DispatchPassThrough;
    }

    DriverObject->MajorFunction[IRP_MJ_PNP] = Ac97DispatchPnp;

    return STATUS_SUCCESS;
}
