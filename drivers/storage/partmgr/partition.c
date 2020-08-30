/*
 * PROJECT:     Partition manager driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Partition device code
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include "partmgr.h"

NTSTATUS
PartMgrReadPartitionTableEx(
    _In_ PFDO_EXTENSION FDOExtension,
    _Out_ PDRIVE_LAYOUT_INFORMATION_EX* DriveLayout)
{
    PDRIVE_LAYOUT_INFORMATION_EX layoutEx = NULL;
    NTSTATUS status = IoReadPartitionTableEx(FDOExtension->LowerDevice, &layoutEx);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    FDOExtension->LayoutCache = layoutEx;
    *DriveLayout = layoutEx;

    return status;
}

NTSTATUS
PartMgrCreatePartitionDevice(
    _In_ PDEVICE_OBJECT FDObject,
    _In_ PPARTITION_INFORMATION_EX PartitionEntry,
    _In_ UINT32 OnDiskNumber,
    _In_ PARTITION_STYLE PartitionStyle,
    _Out_ PDEVICE_OBJECT *PDO)
{
    PAGED_CODE();

    static UINT32 HaddiskVolumeNextId = 1; // this is 1-based

    WCHAR nameBuf[64];
    UNICODE_STRING deviceName;
    // PFDO_EXTENSION fdoExtension = FDObject->DeviceExtension;

    // create the device object

    swprintf(nameBuf, L"\\Device\\HarddiskVolume%u", HaddiskVolumeNextId++);
    RtlCreateUnicodeString(&deviceName, nameBuf);

    PDEVICE_OBJECT partitionDevice;
    NTSTATUS status = IoCreateDevice(
        FDObject->DriverObject,
        sizeof(PARTITION_EXTENSION),
        &deviceName,
        FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &partitionDevice);

    if (!NT_SUCCESS(status))
    {
        ERR("[PARTMGR] Unable to create device object %wZ\n", &deviceName);
        return status;
    }

    INFO("[PARTMGR] Created device object %p %wZ\n", partitionDevice, &deviceName);

    PPARTITION_EXTENSION partExt = partitionDevice->DeviceExtension;
    RtlZeroMemory(partExt, sizeof(*partExt));

    partitionDevice->StackSize = FDObject->StackSize;
    partitionDevice->Flags |= DO_DIRECT_IO;

    if (PartitionStyle == PARTITION_STYLE_MBR)
    {
        partExt->Mbr.PartitionType = PartitionEntry->Mbr.PartitionType;
        partExt->Mbr.BootIndicator = PartitionEntry->Mbr.BootIndicator;
        partExt->Mbr.HiddenSectors = PartitionEntry->Mbr.HiddenSectors;
    }
    else
    {
        partExt->Gpt.PartitionType = PartitionEntry->Gpt.PartitionType;
        partExt->Gpt.PartitionId = PartitionEntry->Gpt.PartitionType;
        partExt->Gpt.Attributes = PartitionEntry->Gpt.Attributes;

        RtlCopyMemory(partExt->Gpt.Name, PartitionEntry->Gpt.Name, sizeof(partExt->Gpt.Name));
    }

    partExt->DeviceName = deviceName;
    partExt->StartingOffset = PartitionEntry->StartingOffset.QuadPart;
    partExt->PartitionLength = PartitionEntry->PartitionLength.QuadPart;
    partExt->OnDiskNumber = OnDiskNumber;
    partExt->DetectedNumber = PartitionEntry->PartitionNumber;

    partExt->DeviceObject = partitionDevice;
    partExt->LowerDevice = FDObject;

    partitionDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    *PDO = partitionDevice;

    return status;
}

NTSTATUS
PartMgrPartitionHandlePnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    PPARTITION_EXTENSION partExt = DeviceObject->DeviceExtension;
    static const WCHAR PartitionSymLinkFormat[] = L"\\Device\\Harddisk%u\\Partition%u";
    NTSTATUS status;

    switch (ioStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            // first, create a symbolic link for our device

            WCHAR nameBuf[64];
            UNICODE_STRING partitionSymlink, interfaceName;
            PFDO_EXTENSION fdoExtension = partExt->LowerDevice->DeviceExtension;

            // \\Device\\Harddisk%u\\Partition%u
            swprintf(nameBuf, PartitionSymLinkFormat,
                fdoExtension->DiskData.DeviceNumber, partExt->DetectedNumber);

            if (!RtlCreateUnicodeString(&partitionSymlink, nameBuf))
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            status = IoCreateSymbolicLink(&partitionSymlink, &partExt->DeviceName);

            if (!NT_SUCCESS(status))
            {
                break;
            }

            TRACE("[PARTMGR] Symlink created %wZ -> %wZ\n", &partExt->DeviceName, &partitionSymlink);

            // our partition device will have two interfaces:
            // GUID_DEVINTERFACE_PARTITION and GUID_DEVINTERFACE_VOLUME
            // the former one is used to notify mountmgr about new device

            status = IoRegisterDeviceInterface(
                DeviceObject, &GUID_DEVINTERFACE_PARTITION, NULL, &interfaceName);

            if (!NT_SUCCESS(status))
            {
                break;
            }

            partExt->PartitionInterfaceName = interfaceName;
            status = IoSetDeviceInterfaceState(&interfaceName, TRUE);

            INFO("[PARTMGR] Partition interface %wZ\n", &interfaceName);

            if (!NT_SUCCESS(status))
            {
                RtlFreeUnicodeString(&interfaceName);
                RtlInitUnicodeString(&partExt->PartitionInterfaceName, NULL);
                break;
            }

            status = IoRegisterDeviceInterface(
                DeviceObject, &GUID_DEVINTERFACE_VOLUME, NULL, &interfaceName);

            if (!NT_SUCCESS(status))
            {
                break;
            }

            partExt->VolumeInterfaceName = interfaceName;
            status = IoSetDeviceInterfaceState(&interfaceName, TRUE);

            INFO("[PARTMGR] Volume interface %wZ\n", &interfaceName);

            if (!NT_SUCCESS(status))
            {
                RtlFreeUnicodeString(&interfaceName);
                RtlInitUnicodeString(&partExt->VolumeInterfaceName, NULL);
                break;
            }

            status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_REMOVE_DEVICE:
        {
            // remove the symbolic link
            WCHAR nameBuf[64];
            UNICODE_STRING partitionSymlink;
            PFDO_EXTENSION fdoExtension = partExt->LowerDevice->DeviceExtension;

            swprintf(nameBuf, PartitionSymLinkFormat,
                fdoExtension->DiskData.DeviceNumber, partExt->DetectedNumber);

            if (!RtlCreateUnicodeString(&partitionSymlink, nameBuf))
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            status = IoDeleteSymbolicLink(&partitionSymlink);

            if (!NT_SUCCESS(status))
            {
                break;
            }

            DPRINT("[PARTMGR] Symlink removed %wZ -> %wZ\n", &partExt->DeviceName, &partitionSymlink);

            // release device interfaces

            if (partExt->PartitionInterfaceName.Buffer)
            {
                IoSetDeviceInterfaceState(&partExt->PartitionInterfaceName, FALSE);
                RtlFreeUnicodeString(&partExt->PartitionInterfaceName);
                RtlInitUnicodeString(&partExt->PartitionInterfaceName, NULL);
            }

            if (partExt->VolumeInterfaceName.Buffer)
            {
                IoSetDeviceInterfaceState(&partExt->VolumeInterfaceName, FALSE);
                RtlFreeUnicodeString(&partExt->VolumeInterfaceName);
                RtlInitUnicodeString(&partExt->VolumeInterfaceName, NULL);
            }

            // PartMgrAcquireLayoutLock(fdoExtension);

            // IoDeleteDevice(DeviceObject);

            // PartMgrReleaseLayoutLock(fdoExtension);

            status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            DEVICE_RELATION_TYPE type = ioStack->Parameters.QueryDeviceRelations.Type;

            if (type == TargetDeviceRelation)
            {
                // Device relations has one entry built in to it's size.

                status = STATUS_INSUFFICIENT_RESOURCES;

                PDEVICE_RELATIONS deviceRelations = ExAllocatePoolZero(PagedPool, sizeof(DEVICE_RELATIONS), TAG_PARTMGR);

                if (deviceRelations != NULL)
                {
                    deviceRelations->Count = 1;
                    deviceRelations->Objects[0] = DeviceObject;
                    ObReferenceObject(deviceRelations->Objects[0]);

                    Irp->IoStatus.Information = (ULONG_PTR) deviceRelations;
                    status = STATUS_SUCCESS;
                }

            }
            else
            {
                Irp->IoStatus.Information = 0;
                status = Irp->IoStatus.Status;
            }
            break;
        }
        case IRP_MN_QUERY_ID:
        {
            BUS_QUERY_ID_TYPE idType = ioStack->Parameters.QueryId.IdType;
            UNICODE_STRING idString;

            switch (idType)
            {
                case BusQueryDeviceID:
                    status = RtlCreateUnicodeString(&idString, L"STORAGE\\Partition")
                            ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
                    break;
                case BusQueryHardwareIDs:
                case BusQueryCompatibleIDs:
                {
                    idString.Buffer = ExAllocatePoolUninitialized(PagedPool, sizeof(L"STORAGE\\Volume\0"), TAG_PARTMGR);
                    RtlCopyMemory(idString.Buffer, L"STORAGE\\Volume\0", sizeof(L"STORAGE\\Volume\0"));

                    status = STATUS_SUCCESS;
                    break;
                }
                case BusQueryInstanceID:
                {
                    WCHAR string[64];
                    PFDO_EXTENSION fdoExtension = partExt->LowerDevice->DeviceExtension;

                    PartMgrAcquireLayoutLock(fdoExtension);

                    PDRIVE_LAYOUT_INFORMATION_EX layoutInfo = fdoExtension->LayoutCache;

                    if (partExt->Style == PARTITION_STYLE_MBR)
                    {
                        swprintf(string, L"S%08lx_O%I64x_L%I64x",
                            layoutInfo->Mbr.Signature, partExt->StartingOffset, partExt->PartitionLength);
                    }
                    else
                    {
                        swprintf(string,
                                L"S%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02xS_O%I64x_L%I64x",
                                layoutInfo->Gpt.DiskId.Data1,
                                layoutInfo->Gpt.DiskId.Data2,
                                layoutInfo->Gpt.DiskId.Data3,
                                layoutInfo->Gpt.DiskId.Data4[0],
                                layoutInfo->Gpt.DiskId.Data4[1],
                                layoutInfo->Gpt.DiskId.Data4[2],
                                layoutInfo->Gpt.DiskId.Data4[3],
                                layoutInfo->Gpt.DiskId.Data4[4],
                                layoutInfo->Gpt.DiskId.Data4[5],
                                layoutInfo->Gpt.DiskId.Data4[6],
                                layoutInfo->Gpt.DiskId.Data4[7],
                                partExt->StartingOffset,
                                partExt->PartitionLength);
                    }

                    PartMgrReleaseLayoutLock(fdoExtension);

                    status = RtlCreateUnicodeString(&idString, string)
                            ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }
                default:
                    status = STATUS_NOT_SUPPORTED;
                    break;
            }

            Irp->IoStatus.Information = NT_SUCCESS(status) ? (ULONG_PTR) idString.Buffer : 0;
            break;
        }
        case IRP_MN_QUERY_CAPABILITIES:
        {
            PDEVICE_CAPABILITIES devCaps = ioStack->Parameters.DeviceCapabilities.Capabilities;
            ASSERT(devCaps);

            // TODO: check if lock is needed

            devCaps->SilentInstall = TRUE;
            devCaps->RawDeviceOK = TRUE;
            devCaps->Address = partExt->OnDiskNumber;
            devCaps->UniqueID = 1; // TODO: 0 for removable device

            status = STATUS_SUCCESS;
            break;
        }
        default:
        {
            Irp->IoStatus.Information = 0;
            status = STATUS_NOT_SUPPORTED;
        }
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS
PartMgrPartitionDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    PPARTITION_EXTENSION partExt = DeviceObject->DeviceExtension;
    PFDO_EXTENSION fdoExtension = partExt->LowerDevice->DeviceExtension;
    NTSTATUS status;

    ASSERT(!partExt->IsFDO);

    /* Need to implement:
    IOCTL_DISK_GET_PARTITION_INFO +
    IOCTL_DISK_SET_PARTITION_INFO
    IOCTL_DISK_GET_PARTITION_INFO_EX +
    IOCTL_DISK_SET_PARTITION_INFO_EX
    IOCTL_DISK_UPDATE_PROPERTIES

    IOCTL_MOUNTDEV_QUERY_DEVICE_NAME +
    IOCTL_MOUNTDEV_QUERY_UNIQUE_ID +
    IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME optional

    IOCTL_VOLUME_GET_GPT_ATTRIBUTES

    IOCTL_DISK_VERIFY // adjust offset +
    IOCTL_DISK_GET_LENGTH_INFO // check if we need to do anything
     */

    switch (ioStack->Parameters.DeviceIoControl.IoControlCode)
    {
        // disk stuff
        case IOCTL_DISK_GET_PARTITION_INFO:
        {
            if (!VerifyIrpBufferSize(Irp, sizeof(PARTITION_INFORMATION)))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            // not supported on anything other than MBR
            if (fdoExtension->DiskData.PartitionStyle != PARTITION_STYLE_MBR)
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            PPARTITION_INFORMATION partInfo = Irp->AssociatedIrp.SystemBuffer;

            PartMgrAcquireLayoutLock(fdoExtension);

            *partInfo = (PARTITION_INFORMATION){
                .PartitionType = partExt->Mbr.PartitionType,
                .StartingOffset.QuadPart = partExt->StartingOffset,
                .PartitionLength.QuadPart = partExt->PartitionLength,
                .HiddenSectors = partExt->Mbr.HiddenSectors,
                .PartitionNumber = partExt->DetectedNumber,
                .BootIndicator = partExt->Mbr.BootIndicator,
                .RecognizedPartition = partExt->Mbr.RecognizedPartition,
                .RewritePartition = FALSE,
            };

            PartMgrReleaseLayoutLock(fdoExtension);

            Irp->IoStatus.Information = sizeof(*partInfo);
            status = STATUS_SUCCESS;
            break;
        }
        case IOCTL_DISK_GET_PARTITION_INFO_EX:
        {
            if (!VerifyIrpBufferSize(Irp, sizeof(PARTITION_INFORMATION_EX)))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PPARTITION_INFORMATION_EX partInfoEx = Irp->AssociatedIrp.SystemBuffer;

            PartMgrAcquireLayoutLock(fdoExtension);

            *partInfoEx = (PARTITION_INFORMATION_EX){
                .StartingOffset.QuadPart = partExt->StartingOffset,
                .PartitionLength.QuadPart = partExt->PartitionLength,
                .PartitionNumber = partExt->DetectedNumber,
                .PartitionStyle = fdoExtension->DiskData.PartitionStyle,
                .RewritePartition = FALSE,
            };

            if (fdoExtension->DiskData.PartitionStyle == PARTITION_STYLE_MBR)
            {
                partInfoEx->Mbr = (PARTITION_INFORMATION_MBR){
                    .PartitionType = partExt->Mbr.PartitionType,
                    .HiddenSectors = partExt->Mbr.HiddenSectors,
                    .BootIndicator = partExt->Mbr.BootIndicator,
                    .RecognizedPartition = partExt->Mbr.RecognizedPartition,
                };
            }
            else
            {
                partInfoEx->Gpt = (PARTITION_INFORMATION_GPT){
                    .PartitionType = partExt->Gpt.PartitionType,
                    .PartitionId = partExt->Gpt.PartitionId,
                    .Attributes = partExt->Gpt.Attributes,
                };

                RtlCopyMemory(partInfoEx->Gpt.Name, partExt->Gpt.Name, sizeof(partInfoEx->Gpt.Name));
            }

            PartMgrReleaseLayoutLock(fdoExtension);

            Irp->IoStatus.Information = sizeof(*partInfoEx);
            status = STATUS_SUCCESS;
            break;
        }
        case IOCTL_DISK_SET_PARTITION_INFO:
        {
            // todo: check input buffer
            PSET_PARTITION_INFORMATION inputBuffer = Irp->AssociatedIrp.SystemBuffer;

            PartMgrAcquireLayoutLock(fdoExtension);

            // these functions use on disk numbers, not detected ones
            status = IoSetPartitionInformation(
                fdoExtension->LowerDevice,
                fdoExtension->DiskData.BytesPerSector,
                partExt->OnDiskNumber,
                inputBuffer->PartitionType);

            if (NT_SUCCESS(status))
            {
                partExt->Mbr.PartitionType = inputBuffer->PartitionType;
            }

            PartMgrReleaseLayoutLock(fdoExtension);

            Irp->IoStatus.Information = 0;
            break;
        }
        case IOCTL_DISK_SET_PARTITION_INFO_EX:
        {
            PSET_PARTITION_INFORMATION_EX inputBuffer = Irp->AssociatedIrp.SystemBuffer;

            PartMgrAcquireLayoutLock(fdoExtension);

            // these functions use on disk numbers, not detected ones
            status = IoSetPartitionInformationEx(
                fdoExtension->LowerDevice,
                partExt->OnDiskNumber,
                inputBuffer);

            if (NT_SUCCESS(status))
            {
                if (fdoExtension->DiskData.PartitionStyle == PARTITION_STYLE_MBR)
                {
                    partExt->Mbr.PartitionType = inputBuffer->Mbr.PartitionType;
                }
                else
                {
                    partExt->Gpt.PartitionType = inputBuffer->Gpt.PartitionType;
                    partExt->Gpt.PartitionId = inputBuffer->Gpt.PartitionId;
                    partExt->Gpt.Attributes = inputBuffer->Gpt.Attributes;

                    RtlMoveMemory(partExt->Gpt.Name, inputBuffer->Gpt.Name, sizeof(partExt->Gpt.Name));
                }
            }

            PartMgrReleaseLayoutLock(fdoExtension);

            Irp->IoStatus.Information = 0;
            break;
        }
        case IOCTL_DISK_VERIFY:
        {
            PVERIFY_INFORMATION verifyInfo = Irp->AssociatedIrp.SystemBuffer;
            if (ioStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(VERIFY_INFORMATION))
            {
                status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            // Partition device should just adjust the starting offset
            verifyInfo->StartingOffset.QuadPart += partExt->StartingOffset;
            return ForwardIrpAndForget(DeviceObject, Irp);
        }
        // mountmgr stuff
        case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
        {
            PMOUNTDEV_NAME name;

            if (ioStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
                break;
            }

            name = Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(name, sizeof(MOUNTDEV_NAME));
            name->NameLength = partExt->DeviceName.Length;

            if (ioStack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(USHORT) + name->NameLength)
            {
                status = STATUS_BUFFER_OVERFLOW;
                Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
                break;
            }

            RtlCopyMemory(name->Name, partExt->DeviceName.Buffer, name->NameLength);

            status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(USHORT) + name->NameLength;
            break;
        }
        case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
        {
            PMOUNTDEV_UNIQUE_ID uniqueId;

            // if (!commonExtension->MountedDeviceInterfaceName.Buffer)
            // {
            //     status = STATUS_INVALID_PARAMETER;
            //     break;
            // }

            if (ioStack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(MOUNTDEV_UNIQUE_ID))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
                break;
            }

            uniqueId = Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(uniqueId, sizeof(MOUNTDEV_UNIQUE_ID));
            uniqueId->UniqueIdLength = partExt->VolumeInterfaceName.Length;

            if (ioStack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(USHORT) + uniqueId->UniqueIdLength)
            {

                status = STATUS_BUFFER_OVERFLOW;
                Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
                break;
            }

            RtlCopyMemory(uniqueId->UniqueId,
                          partExt->VolumeInterfaceName.Buffer,
                          uniqueId->UniqueIdLength);

            status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(USHORT) + uniqueId->UniqueIdLength;
            break;
        }
        default:
            return ForwardIrpAndForget(DeviceObject, Irp);
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
