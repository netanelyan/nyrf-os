/* Minimal UEFI definitions used by the nyrf OS bootloader.
 *
 * The research document selected POSIX-UEFI as the wrapper library. For the
 * POC we vendor the handful of types and protocols we actually touch instead:
 * it keeps the repository self-contained (no download step in the build), and
 * every structure below is transcribed from the UEFI 2.10 specification, so
 * swapping in POSIX-UEFI or EDK2 later is a header change and nothing more.
 *
 * Field order inside the service tables is part of the ABI. Unused entries are
 * kept as void* placeholders on purpose - they must never be removed.
 */
#ifndef NYRF_UEFI_H
#define NYRF_UEFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  BOOLEAN;
typedef int64_t  INTN;
typedef uint64_t UINTN;
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int16_t  INT16;
typedef uint16_t CHAR16;
typedef void     VOID;

typedef UINTN    EFI_STATUS;
typedef VOID    *EFI_HANDLE;
typedef VOID    *EFI_EVENT;
typedef UINT64   EFI_PHYSICAL_ADDRESS;
typedef UINT64   EFI_VIRTUAL_ADDRESS;

/* The UEFI calling convention is the Microsoft x64 one, whatever ABI the rest
 * of the translation unit is compiled with. */
#define EFIAPI __attribute__((ms_abi))

#define EFI_ERROR_BIT           (1ULL << 63)
#define EFI_ERROR(s)            (((EFI_STATUS)(s)) & EFI_ERROR_BIT)
#define EFI_SUCCESS             0ULL
#define EFI_LOAD_ERROR          (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER   (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED         (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL    (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* --------------------------------------------------------------- memory --- */

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------- text out --- */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    VOID *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    VOID *SetCursorPosition;
    VOID *EnableCursor;
    VOID *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* -------------------------------------------------------- boot services --- */

typedef struct {
    EFI_TABLE_HEADER Hdr;

    /* Task priority */
    VOID *RaiseTPL;
    VOID *RestoreTPL;

    /* Memory */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(VOID *Buffer);

    /* Event and timer */
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;

    /* Protocol handler */
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;

    /* Image */
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    /* Miscellaneous */
    VOID *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                          UINTN DataSize, CHAR16 *WatchdogData);

    /* Driver support */
    VOID *ConnectController;
    VOID *DisconnectController;

    /* Open / close protocol */
    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;

    /* Library */
    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;

    /* CRC32 */
    VOID *CalculateCrc32;

    /* Memory helpers */
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ------------------------------------------------------------------ GOP --- */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    VOID                              *QueryMode;
    VOID                              *SetMode;
    VOID                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---------------------------------------------------------- loaded image -- */

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle;
    VOID             *FilePath;
    VOID             *Reserved;
    UINT32            LoadOptionsSize;
    VOID             *LoadOptions;
    VOID             *ImageBase;
    UINT64            ImageSize;
    EFI_MEMORY_TYPE   ImageCodeType;
    EFI_MEMORY_TYPE   ImageDataType;
    VOID             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ----------------------------------------------------------- file system -- */

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This,
                              struct EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    VOID *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
    VOID *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
    VOID *GetInfo;
    VOID *SetInfo;
    VOID *Flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ----------------------------------------------------------------- GUIDs -- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#define EFI_PAGE_SIZE 4096ULL

#endif /* NYRF_UEFI_H */
