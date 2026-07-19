#include <ntddk.h>

#define ICH_REG_X_SR 0x06
#define ICH_REG_X_CR 0x0b
#define ICH_REG_PI_BASE 0x00
#define ICH_REG_PO_BASE 0x10
#define ICH_REG_MC_BASE 0x20
#define ICH_REG_GLOB_CNT 0x2c
#define ICH_REG_GLOB_STA 0x30
#define ICH_REG_ACC_SEMA 0x34

#define ICH_X_SR_DCH 0x0001
#define ICH_X_SR_CELV 0x0002
#define ICH_X_SR_LVBCI 0x0004
#define ICH_X_SR_BCIS 0x0008
#define ICH_X_SR_FIFOE 0x0010

#define ICH_X_CR_RR 0x02

#define ICH_GLOB_CTL_COLD 0x00000002
#define ICH_GLOB_CTL_WARM 0x00000004
#define ICH_GLOB_CTL_SHUT 0x00000008

#define ICH_GLOB_STA_PCR 0x00000100

#define AC97_CODEC_TIMEOUT 1000
#define AC97_RESET_TIMEOUT 500000

typedef enum _AC97_RESOURCE_TYPE {
    Ac97ResourceTypeNone,
    Ac97ResourceTypePort,
    Ac97ResourceTypeMemory
} AC97_RESOURCE_TYPE;

typedef struct _AC97_DEVICE_EXTENSION {
    PDEVICE_OBJECT LowerDevice;
    PDEVICE_OBJECT PhysicalDeviceObject;
    BOOLEAN Started;
    BOOLEAN Removing;
    AC97_RESOURCE_TYPE ResourceType;
    PHYSICAL_ADDRESS MixerBasePhysical;
    ULONG MixerLength;
    PUCHAR MixerBase;
    PHYSICAL_ADDRESS BusMasterBasePhysical;
    ULONG BusMasterLength;
    PUCHAR BusMasterBase;
} AC97_DEVICE_EXTENSION, *PAC97_DEVICE_EXTENSION;

DRIVER_UNLOAD Ac97Unload;
DRIVER_ADD_DEVICE Ac97AddDevice;
_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH Ac97DispatchPnp;
_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH Ac97DispatchPower;
IO_COMPLETION_ROUTINE Ac97CompletionRoutine;

static NTSTATUS Ac97DispatchPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
static NTSTATUS Ac97CaptureResources(_Inout_ PAC97_DEVICE_EXTENSION Extension, _In_ PCM_RESOURCE_LIST Resources);
static NTSTATUS Ac97MapResources(_Inout_ PAC97_DEVICE_EXTENSION Extension);
static VOID Ac97UnmapResources(_Inout_ PAC97_DEVICE_EXTENSION Extension);
static NTSTATUS Ac97InitializeHardware(_Inout_ PAC97_DEVICE_EXTENSION Extension);
static NTSTATUS Ac97ResetController(_Inout_ PAC97_DEVICE_EXTENSION Extension);
static NTSTATUS Ac97ResetChannels(_Inout_ PAC97_DEVICE_EXTENSION Extension);
static NTSTATUS Ac97WaitCodecReady(_In_ PAC97_DEVICE_EXTENSION Extension);
static UCHAR Ac97Read8(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset);
static USHORT Ac97Read16(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset);
static ULONG Ac97Read32(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset);
static VOID Ac97Write8(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ UCHAR Value);
static VOID Ac97Write16(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ USHORT Value);
static VOID Ac97Write32(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ ULONG Value);

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

static NTSTATUS
Ac97DispatchPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(extension->LowerDevice, Irp);
}

static UCHAR
Ac97Read8(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return 0xff;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        return READ_REGISTER_UCHAR(base + Offset);
    }

    return READ_PORT_UCHAR(base + Offset);
}

static USHORT
Ac97Read16(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return 0xffff;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        return READ_REGISTER_USHORT((PUSHORT)(base + Offset));
    }

    return READ_PORT_USHORT((PUSHORT)(base + Offset));
}

static ULONG
Ac97Read32(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return 0xffffffff;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        return READ_REGISTER_ULONG((PULONG)(base + Offset));
    }

    return READ_PORT_ULONG((PULONG)(base + Offset));
}

static VOID
Ac97Write8(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ UCHAR Value)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        WRITE_REGISTER_UCHAR(base + Offset, Value);
        return;
    }

    WRITE_PORT_UCHAR(base + Offset, Value);
}

static VOID
Ac97Write16(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ USHORT Value)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        WRITE_REGISTER_USHORT((PUSHORT)(base + Offset), Value);
        return;
    }

    WRITE_PORT_USHORT((PUSHORT)(base + Offset), Value);
}

static VOID
Ac97Write32(_In_ PAC97_DEVICE_EXTENSION Extension, _In_ BOOLEAN BusMaster, _In_ ULONG Offset, _In_ ULONG Value)
{
    PUCHAR base = BusMaster ? Extension->BusMasterBase : Extension->MixerBase;

    if (base == NULL) {
        return;
    }

    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        WRITE_REGISTER_ULONG((PULONG)(base + Offset), Value);
        return;
    }

    WRITE_PORT_ULONG((PULONG)(base + Offset), Value);
}

static NTSTATUS
Ac97CaptureResources(_Inout_ PAC97_DEVICE_EXTENSION Extension, _In_ PCM_RESOURCE_LIST Resources)
{
    ULONG fullIndex;
    ULONG index;
    ULONG barCount = 0;

    Extension->ResourceType = Ac97ResourceTypeNone;
    Extension->MixerBasePhysical.QuadPart = 0;
    Extension->MixerLength = 0;
    Extension->MixerBase = NULL;
    Extension->BusMasterBasePhysical.QuadPart = 0;
    Extension->BusMasterLength = 0;
    Extension->BusMasterBase = NULL;

    if (Resources == NULL || Resources->Count == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (fullIndex = 0; fullIndex < Resources->Count && barCount < 2; fullIndex++) {
        PCM_PARTIAL_RESOURCE_LIST partialResourceList = &Resources->List[fullIndex].PartialResourceList;

        for (index = 0; index < partialResourceList->Count; index++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor = &partialResourceList->PartialDescriptors[index];
            AC97_RESOURCE_TYPE descriptorType;
            PHYSICAL_ADDRESS start;
            ULONG length;

            if (descriptor->Type == CmResourceTypePort) {
                descriptorType = Ac97ResourceTypePort;
                start = descriptor->u.Port.Start;
                length = descriptor->u.Port.Length;
            } else if (descriptor->Type == CmResourceTypeMemory) {
                descriptorType = Ac97ResourceTypeMemory;
                start = descriptor->u.Memory.Start;
                length = descriptor->u.Memory.Length;
            } else {
                continue;
            }

            if (length == 0) {
                continue;
            }

            if (Extension->ResourceType == Ac97ResourceTypeNone) {
                Extension->ResourceType = descriptorType;
            } else if (Extension->ResourceType != descriptorType) {
                continue;
            }

            if (barCount == 0) {
                Extension->MixerBasePhysical = start;
                Extension->MixerLength = length;
                barCount++;
            } else if (barCount == 1) {
                Extension->BusMasterBasePhysical = start;
                Extension->BusMasterLength = length;
                barCount++;
                break;
            }
        }
    }

    if (barCount < 2) {
        Extension->ResourceType = Ac97ResourceTypeNone;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Ac97MapResources(_Inout_ PAC97_DEVICE_EXTENSION Extension)
{
    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        Extension->MixerBase = (PUCHAR)MmMapIoSpace(Extension->MixerBasePhysical, Extension->MixerLength, MmNonCached);
        if (Extension->MixerBase == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Extension->BusMasterBase = (PUCHAR)MmMapIoSpace(Extension->BusMasterBasePhysical, Extension->BusMasterLength, MmNonCached);
        if (Extension->BusMasterBase == NULL) {
            MmUnmapIoSpace(Extension->MixerBase, Extension->MixerLength);
            Extension->MixerBase = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        return STATUS_SUCCESS;
    }

    if (Extension->ResourceType == Ac97ResourceTypePort) {
        Extension->MixerBase = (PUCHAR)(ULONG_PTR)Extension->MixerBasePhysical.QuadPart;
        Extension->BusMasterBase = (PUCHAR)(ULONG_PTR)Extension->BusMasterBasePhysical.QuadPart;
        return STATUS_SUCCESS;
    }

    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static VOID
Ac97UnmapResources(_Inout_ PAC97_DEVICE_EXTENSION Extension)
{
    if (Extension->ResourceType == Ac97ResourceTypeMemory) {
        if (Extension->BusMasterBase != NULL) {
            MmUnmapIoSpace(Extension->BusMasterBase, Extension->BusMasterLength);
        }

        if (Extension->MixerBase != NULL) {
            MmUnmapIoSpace(Extension->MixerBase, Extension->MixerLength);
        }
    }

    Extension->MixerBase = NULL;
    Extension->BusMasterBase = NULL;
    Extension->MixerLength = 0;
    Extension->BusMasterLength = 0;
    Extension->MixerBasePhysical.QuadPart = 0;
    Extension->BusMasterBasePhysical.QuadPart = 0;
    Extension->ResourceType = Ac97ResourceTypeNone;
}

static NTSTATUS
Ac97WaitCodecReady(_In_ PAC97_DEVICE_EXTENSION Extension)
{
    ULONG index;

    if (Extension->BusMasterLength < ICH_REG_ACC_SEMA + sizeof(UCHAR)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (index = 0; index < AC97_CODEC_TIMEOUT; index++) {
        if ((Ac97Read8(Extension, TRUE, ICH_REG_ACC_SEMA) & 0x01) == 0) {
            return STATUS_SUCCESS;
        }

        KeStallExecutionProcessor(1);
    }

    return STATUS_IO_TIMEOUT;
}

static NTSTATUS
Ac97ResetController(_Inout_ PAC97_DEVICE_EXTENSION Extension)
{
    ULONG control;
    ULONG index;

    if (Extension->BusMasterLength < ICH_REG_GLOB_STA + sizeof(ULONG)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    control = Ac97Read32(Extension, TRUE, ICH_REG_GLOB_CNT);
    control &= ~ICH_GLOB_CTL_SHUT;
    control |= ((control & ICH_GLOB_CTL_COLD) != 0) ? ICH_GLOB_CTL_WARM : ICH_GLOB_CTL_COLD;
    Ac97Write32(Extension, TRUE, ICH_REG_GLOB_CNT, control);

    for (index = 0; index < AC97_RESET_TIMEOUT; index++) {
        if ((Ac97Read32(Extension, TRUE, ICH_REG_GLOB_STA) & ICH_GLOB_STA_PCR) != 0) {
            return Ac97WaitCodecReady(Extension);
        }

        KeStallExecutionProcessor(1);
    }

    return STATUS_IO_TIMEOUT;
}

static NTSTATUS
Ac97ResetChannels(_Inout_ PAC97_DEVICE_EXTENSION Extension)
{
    static const ULONG channelBases[] = { ICH_REG_PO_BASE, ICH_REG_PI_BASE, ICH_REG_MC_BASE };
    ULONG channelIndex;

    for (channelIndex = 0; channelIndex < RTL_NUMBER_OF(channelBases); channelIndex++) {
        ULONG registerBase = channelBases[channelIndex];
        ULONG poll;

        if (Extension->BusMasterLength < registerBase + ICH_REG_X_CR + sizeof(UCHAR)) {
            continue;
        }

        Ac97Write8(Extension, TRUE, registerBase + ICH_REG_X_CR, 0);
        KeStallExecutionProcessor(100);
        Ac97Write8(Extension, TRUE, registerBase + ICH_REG_X_CR, ICH_X_CR_RR);

        for (poll = 0; poll < AC97_CODEC_TIMEOUT; poll++) {
            if (Ac97Read8(Extension, TRUE, registerBase + ICH_REG_X_CR) == 0) {
                break;
            }

            KeStallExecutionProcessor(1);
        }

        if (poll == AC97_CODEC_TIMEOUT) {
            return STATUS_IO_TIMEOUT;
        }

        Ac97Write16(Extension, TRUE, registerBase + ICH_REG_X_SR, ICH_X_SR_DCH | ICH_X_SR_CELV | ICH_X_SR_LVBCI | ICH_X_SR_BCIS | ICH_X_SR_FIFOE);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Ac97InitializeHardware(_Inout_ PAC97_DEVICE_EXTENSION Extension)
{
    NTSTATUS status;

    status = Ac97ResetController(Extension);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return Ac97ResetChannels(Extension);
}

NTSTATUS
Ac97DispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (irpStack->MinorFunction) {
    case IRP_MN_START_DEVICE: {
        NTSTATUS status = Ac97ForwardAndWait(DeviceObject, Irp);

        if (NT_SUCCESS(status)) {
            status = Ac97CaptureResources(extension, irpStack->Parameters.StartDevice.AllocatedResourcesTranslated);
            if (NT_SUCCESS(status)) {
                status = Ac97MapResources(extension);
            }
            if (NT_SUCCESS(status)) {
                status = Ac97InitializeHardware(extension);
            }
            if (NT_SUCCESS(status)) {
                extension->Started = TRUE;
            } else {
                Ac97UnmapResources(extension);
            }
        }

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    case IRP_MN_STOP_DEVICE:
        extension->Started = FALSE;
        Ac97UnmapResources(extension);
        return Ac97DispatchPassThrough(DeviceObject, Irp);

    case IRP_MN_SURPRISE_REMOVAL:
        extension->Started = FALSE;
        Ac97UnmapResources(extension);
        return Ac97DispatchPassThrough(DeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE: {
        NTSTATUS status;

        extension->Removing = TRUE;
        extension->Started = FALSE;
        Ac97UnmapResources(extension);

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
Ac97DispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAC97_DEVICE_EXTENSION extension = (PAC97_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(extension->LowerDevice, Irp);
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
    extension->PhysicalDeviceObject = PhysicalDeviceObject;
    extension->LowerDevice = IoAttachDeviceToDeviceStack(deviceObject, PhysicalDeviceObject);
    if (extension->LowerDevice == NULL) {
        IoDeleteDevice(deviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    deviceObject->Flags |= DO_POWER_PAGABLE;
    deviceObject->Flags |= extension->LowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
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
    DriverObject->MajorFunction[IRP_MJ_POWER] = Ac97DispatchPower;

    return STATUS_SUCCESS;
}
