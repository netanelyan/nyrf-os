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
 *
 * How to read this file: a UEFI "protocol" is just a struct of function
 * pointers - a vtable the firmware hands us. We never construct one; we ask
 * the firmware for a pointer to one and call through it. So the entire file is
 * layout description, not logic: the firmware built these structs before our
 * code existed, and getting a field order wrong means calling the wrong
 * function, not a compile error.
 */
#ifndef NYRF_UEFI_H
#define NYRF_UEFI_H

#include <stdint.h>
#include <stddef.h>

/* UEFI spells its own types. Mapping them onto stdint.h ones keeps the
 * transcribed declarations below looking like the specification while staying
 * fixed-width. INTN/UINTN are the native word: 64 bit on x86_64, and the
 * reason a pointer can be cast through UINTN without loss. */
typedef uint8_t  BOOLEAN;
typedef int64_t  INTN;
typedef uint64_t UINTN;
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int16_t  INT16;
/* UEFI strings are UTF-16, not char. This is what -fshort-wchar is for. */
typedef uint16_t CHAR16;
typedef void     VOID;

typedef UINTN    EFI_STATUS;
/* Handles are opaque firmware cookies. We only ever pass them back. */
typedef VOID    *EFI_HANDLE;
typedef VOID    *EFI_EVENT;
typedef UINT64   EFI_PHYSICAL_ADDRESS;
typedef UINT64   EFI_VIRTUAL_ADDRESS;

/* The UEFI calling convention is the Microsoft x64 one, whatever ABI the rest
 * of the translation unit is compiled with. */
#define EFIAPI __attribute__((ms_abi))

/* EFI_STATUS is not an enum of small integers: the top bit means "error", and
 * the low bits are the code. That is why EFI_ERROR is a single bit test rather
 * than a comparison, and why a success code can be non-zero (warnings have the
 * top bit clear). Always test with EFI_ERROR, never `status != EFI_SUCCESS`. */
#define EFI_ERROR_BIT           (1ULL << 63)
#define EFI_ERROR(s)            (((EFI_STATUS)(s)) & EFI_ERROR_BIT)
#define EFI_SUCCESS             0ULL
#define EFI_LOAD_ERROR          (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER   (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED         (EFI_ERROR_BIT | 3)
/* Returned by GetMemoryMap when the buffer is too small - and it is the
 * *expected* result of the first probing call, not a failure. */
#define EFI_BUFFER_TOO_SMALL    (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)

/* A 128-bit identifier naming a protocol. The layout is the mixed-endian one
 * inherited from Microsoft: Data1/2/3 are little-endian integers, Data4 is a
 * plain byte array. That is why guid_equal() in boot/main.c compares the three
 * integers and then loops over the eight bytes, instead of memcmp-ing 16
 * bytes - the struct may carry padding and the halves are not the same kind of
 * thing. */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* Prefix on every UEFI table. Signature and CRC32 let firmware validate a
 * table it is handed; we only ever read tables, so we ignore all of it. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* --------------------------------------------------------------- memory --- */

/* Which of the three AllocatePages strategies to use. AllocateAnyPages lets
 * the firmware choose (used for the BootInfo page); AllocateAddress demands a
 * specific physical address and is what loading an ET_EXEC kernel at its fixed
 * p_paddr requires - see boot/elf.c. */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* What a region of memory is being used for. The ordering is the spec's and is
 * part of the ABI, so these must stay in place.
 *
 * The distinction that matters to us is which types become free once
 * ExitBootServices is called: EfiConventionalMemory is already free, and
 * EfiBootServicesCode/Data plus EfiLoaderCode become free the moment the
 * firmware is gone - which is exactly the set report_memory_map() in
 * boot/main.c counts as "usable after boot services".
 *
 * EfiLoaderData is the type we allocate *as*, because the firmware will not
 * reclaim it: the kernel image, the memory map buffer and the BootInfo page
 * all have to outlive boot services. */
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

/* One entry of the memory map: a run of NumberOfPages 4 KiB pages starting at
 * PhysicalStart, all of one Type.
 *
 * The trap: the firmware reports its own descriptor size through
 * GetMemoryMap's DescriptorSize, and it is allowed to be *larger* than this
 * struct - the spec lets an implementation append fields. So the array must be
 * walked in desc_size steps, never with array indexing or sizeof. Both
 * report_memory_map() and the future memory manager depend on this.
 *
 * Pad exists only to align PhysicalStart to 8 bytes. VirtualStart stays zero
 * unless SetVirtualAddressMap is called, which we never do. */
typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------- text out --- */

/* The firmware's text console, reached through SystemTable->ConOut. Only
 * OutputString and ClearScreen are declared as callable; the rest are void*
 * placeholders holding the ABI slots open.
 *
 * This whole protocol disappears at ExitBootServices, which is why con_print()
 * exists only to mirror the serial log for a human watching the screen. */
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

/* Everything the firmware can do for us before we throw it away. Reached
 * through SystemTable->BootServices.
 *
 * The group comments below are the specification's own grouping, kept because
 * they are the only way to check a transcription against the spec by eye. Each
 * void* is a function we never call - but its slot still has to occupy the
 * right number of bytes, or every later field would resolve to the wrong
 * pointer. Adding a service means replacing its placeholder in situ, never
 * appending. */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    /* Task priority */
    VOID *RaiseTPL;
    VOID *RestoreTPL;

    /* Memory.
     *
     * AllocatePages works in 4 KiB pages and can be told an exact address;
     * AllocatePool is a byte-granular malloc. Both take a memory *type*, which
     * is how an allocation is marked to survive ExitBootServices.
     *
     * GetMemoryMap is the awkward one: it reports its own DescriptorSize, and
     * returns a MapKey that ExitBootServices demands back. Any allocation
     * invalidates that key - including an allocation made to hold the map. */
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

    /* Protocol handler.
     *
     * HandleProtocol asks "does *this* handle speak that protocol?" - used for
     * our own image handle and for the volume we were loaded from. Reserved is
     * a genuine dead slot in the spec, not an omission of ours. */
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;

    /* Image.
     *
     * ExitBootServices is the point of no return: it tears down the firmware's
     * drivers and every protocol above. It fails with INVALID_PARAMETER if
     * MapKey is stale, which is what the retry loop in boot/main.c handles. */
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    /* Miscellaneous.
     *
     * SetWatchdogTimer(0,...) disarms the firmware's five-minute reset - the
     * one line standing between us and a reboot mid-demo. */
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

    /* Library.
     *
     * LocateProtocol asks "does *anything* speak that protocol?" and returns
     * the first match - the right call for the framebuffer, where we want a
     * GOP and do not care which device provides it. */
    VOID *ProtocolsPerHandle;
    VOID *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;

    /* CRC32 */
    VOID *CalculateCrc32;

    /* Memory helpers.
     *
     * The firmware's own CopyMem/SetMem. We define memcpy/memset in
     * boot/main.c instead, because the compiler emits calls to those by name
     * and cannot be told to route them through a function pointer. */
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

/* One entry of the system table's configuration array: a GUID naming what the
 * table is, and an untyped pointer to it. This is how the ACPI RSDP is found -
 * by scanning for the ACPI GUID rather than searching memory for a signature,
 * which is the legacy BIOS technique. */
typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* The root of everything. The firmware passes a pointer to this as efi_main's
 * second argument, and every other capability is reached from here.
 *
 * ConIn and RuntimeServices are void* because we never touch them. Runtime
 * services would be the interesting one to fill in later: unlike boot
 * services, they survive ExitBootServices. */
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

/* How the bytes of a pixel are arranged in the framebuffer.
 *
 * The two Reserved8BitPerColor formats are 32 bits per pixel with one byte
 * unused. PixelBitMask means "read PixelInformation to find out" - the general
 * case, which normalise_pixel_format() only accepts when the masks happen to
 * match one of our two layouts.
 *
 * PixelBltOnly is the fatal one: it means there is no linear framebuffer in
 * memory at all and the only way to draw is calling GOP->Blt - a boot service,
 * gone by the time the kernel runs. A machine reporting it cannot run this
 * POC, so the bootloader stops rather than hand the kernel a null screen. */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

/* Which bits of a 32-bit pixel belong to which channel. Only meaningful when
 * PixelFormat is PixelBitMask. */
typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

/* The geometry of the current video mode.
 *
 * HorizontalResolution is what is *visible*; PixelsPerScanLine is what one row
 * actually costs in memory, and may be larger for alignment. Confusing the two
 * skews every row after the first - the mistake the white frame in
 * kernel/main.c is designed to expose. Both are counts of pixels, not bytes. */
typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* The current state of the graphics device. FrameBufferBase is a physical
 * address of real video memory, so writing to it puts pixels on the screen
 * with no firmware call involved - which is precisely why the kernel can still
 * draw after ExitBootServices, and why these two fields are the payload of
 * boot_info_t. */
typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* The graphics protocol itself. We use it purely as a way to *ask* about the
 * framebuffer - QueryMode, SetMode and Blt stay placeholders because the POC
 * accepts whatever mode the firmware has already set. */
typedef struct {
    VOID                              *QueryMode;
    VOID                              *SetMode;
    VOID                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---------------------------------------------------------- loaded image -- */

/* What the firmware knows about *us*, obtained by asking our own image handle
 * for this protocol.
 *
 * DeviceHandle is the field that matters: it is the volume this executable was
 * loaded from, i.e. the ESP. Asking it for a filesystem is how kernel.elf is
 * found next to BOOTX64.EFI without hardcoding a disk or partition number.
 *
 * ImageBase and ImageSize are logged only as a debugging convenience - they
 * tell you where the firmware relocated us, which is otherwise invisible. */
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

/* An open file or directory. The same struct is both, which is why Open is
 * called on a directory handle to get a file handle.
 *
 * Read takes BufferSize as in/out: on return it holds how much was actually
 * read, which may be less than asked for. read_at() in boot/elf.c loops for
 * exactly that reason. Paths are CHAR16 and backslash-separated. */
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

/* A mounted filesystem. Its only job here is OpenVolume, which yields the root
 * directory handle that \kernel.elf is opened relative to. */
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ----------------------------------------------------------------- GUIDs -- */

/* Well-known constants from the specification, not values we chose. Each one
 * is the name we pass to LocateProtocol or HandleProtocol to ask for the
 * matching struct above. They are macros rather than objects because a GUID
 * has to be copied into a local before its address can be taken - the
 * firmware's signatures want a non-const EFI_GUID *. */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
/* Two ACPI GUIDs because a machine may publish either or both. 2.0+ gives an
 * XSDT with 64-bit table pointers; 1.0 only an RSDT. find_rsdp() prefers the
 * former and keeps the latter as a fallback. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* UEFI's page size is fixed at 4 KiB regardless of what the CPU supports, so
 * page counts from AllocatePages and NumberOfPages both scale by this. */
#define EFI_PAGE_SIZE 4096ULL

#endif /* NYRF_UEFI_H */
