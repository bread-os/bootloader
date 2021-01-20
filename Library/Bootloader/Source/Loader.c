#include <elf_common.h>
#include <elf64.h>

#include <Uefi/UefiBaseType.h>
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/Acpi.h>

CHAR16 *gKernelPath = L"kernel.efi";
CHAR16 *gKernelEntry = L"kernel_start";
CHAR16 *gFontPath = L"kernel-font.psf";

// @todo: remove these structs
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
typedef struct
{
  void *BaseAddress;
  UINTN BufferSize;
  unsigned int Width;
  unsigned int Height;
  unsigned int PixelsPerScanLine;
} FrameBuffer;

typedef struct
{
  unsigned char magic[2];
  unsigned char mode;
  unsigned char charsize;
} PSF1_HEADER;

typedef struct
{
  PSF1_HEADER *psf1_Header;
  void *graphBuffer;
} PSF1_FONT;

typedef struct
{
  FrameBuffer *framebuffer;
  PSF1_FONT *psf_1_font;
  void *mMap;
  UINTN mMapSize;
  UINTN mMapDescriptorSize;
  void *rsdp;
} BootInfo;
FrameBuffer frameBuffer;
BootInfo bootInfo;

EFI_FILE *LoadFile(IN EFI_FILE *Directory, IN CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_FILE *File;
  EFI_HANDLE_PROTOCOL HandleProtocol = SystemTable->BootServices->HandleProtocol;

  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  HandleProtocol(ImageHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&LoadedImage);

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);

  EFI_FILE *Root;
  if (Directory == NULL)
    FileSystem->OpenVolume(FileSystem, &Root);
  else
    Root = Directory;
  EFI_STATUS Status = Root->Open(Root, &File, gKernelPath, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
  if (Status != EFI_SUCCESS)
  {
    ErrorPrint(L"Kernel file missing\r\n");
    return NULL;
  }
  return File;
}

PSF1_FONT *LoadPSF1Font(IN EFI_FILE *Directory, IN CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_FILE *Font = LoadFile(Directory, Path, ImageHandle, SystemTable);
  if (Font == NULL)
    return NULL;
  PSF1_HEADER *FontHeader;
  UINTN Size = sizeof(PSF1_HEADER);
  SystemTable->BootServices->AllocatePool(EfiLoaderData, Size, (VOID **)&FontHeader);
  Font->Read(Font, &Size, FontHeader);

  if (FontHeader->magic[0] != PSF1_MAGIC0 || FontHeader->magic[1] != PSF1_MAGIC1)
  {
    return NULL;
  }
  UINTN GraphBufferSize = FontHeader->charsize * 256;
  if (FontHeader->mode == 1)
  {
    GraphBufferSize = FontHeader->charsize * 512;
  }
  VOID *GraphBuffer;
  {
    Font->SetPosition(Font, Size);
    SystemTable->BootServices->AllocatePool(EfiLoaderData, GraphBufferSize, (VOID **)&GraphBuffer);
    Font->Read(Font, &GraphBufferSize, GraphBuffer);
  }
  PSF1_FONT *ResultFont;
  SystemTable->BootServices->AllocatePool(EfiLoaderData, Size, (VOID **)&ResultFont);
  ResultFont->psf1_Header = FontHeader;
  ResultFont->graphBuffer = GraphBuffer;
  return ResultFont;
}

FrameBuffer *InitGraphicsOutputProtocol(IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GOP;
  EFI_STATUS Status;
  Status = SystemTable->BootServices->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&GOP);
  if (EFI_ERROR(Status))
  {
    ErrorPrint(L"Cannot locate EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID\r\n");
    return NULL;
  }
  frameBuffer.BaseAddress = (VOID *)GOP->Mode->FrameBufferBase;
  frameBuffer.BufferSize = GOP->Mode->FrameBufferSize;
  frameBuffer.Width = GOP->Mode->Info->HorizontalResolution;
  frameBuffer.Height = GOP->Mode->Info->VerticalResolution;
  frameBuffer.PixelsPerScanLine = GOP->Mode->Info->PixelsPerScanLine;
  return &frameBuffer;
}

/**
  as the real entry point for the application.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

**/
EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable)
{
  SystemTable->ConOut->ClearScreen(SystemTable->ConOut);

  EFI_FILE *Kernel = LoadFile(NULL, gKernelPath, ImageHandle, SystemTable);
  {
    EFI_HANDLE_PROTOCOL HandleProtocol = SystemTable->BootServices->HandleProtocol;

    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    HandleProtocol(ImageHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&LoadedImage);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);

    EFI_FILE *Root;
    FileSystem->OpenVolume(FileSystem, &Root);
    EFI_STATUS Status = Root->Open(Root, &Kernel, gKernelPath, EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (Status != EFI_SUCCESS)
    {
      ErrorPrint(L"Kernel file missing\r\n");
      return Status;
    }
  }
  EFI_ALLOCATE_POOL AllocatePool = SystemTable->BootServices->AllocatePool;

  // Load the ELF header
  Elf64_Ehdr Header;
  {
    UINTN FileInfoSize;
    EFI_FILE_INFO *FileInfo;
    Kernel->GetInfo(Kernel, &gEfiFileInfoGuid, &FileInfoSize, NULL);
    AllocatePool(EfiLoaderData, FileInfoSize, (VOID **)&FileInfo);

    UINTN HeaderSize = sizeof(Header);
    Kernel->Read(Kernel, &HeaderSize, &Header);
  }

  if (
      CompareMem(&Header.e_ident[EI_MAG0], ELFMAG, SELFMAG) != 0 ||
      Header.e_ident[EI_CLASS] != ELFCLASS64 ||
      Header.e_ident[EI_DATA] != ELFDATA2LSB ||
      Header.e_type != ET_EXEC ||
      Header.e_machine != EM_AMD64 ||
      Header.e_version != EV_CURRENT)
  {
    ErrorPrint(L"Bad Kernel Format\r\n");
    return RETURN_UNSUPPORTED;
  }

  Elf64_Phdr *Phdrs;
  { // Load Segment Headers
    Kernel->SetPosition(Kernel, Header.e_phoff);
    UINTN Size = Header.e_phnum * Header.e_phentsize;
    AllocatePool(EfiLoaderData, Size, (void **)&Phdrs);
    Kernel->Read(Kernel, &Size, Phdrs);
  }
  EFI_ALLOCATE_PAGES AllocatePage = SystemTable->BootServices->AllocatePages;
  Elf64_Phdr *Phdr;
  for (Phdr = Phdrs;
       (CHAR8 *)Phdr < (CHAR8 *)Phdrs + Header.e_phnum + Header.e_shentsize;
       Phdr = (Elf64_Phdr *)(CHAR8 *)Phdr + Header.e_phentsize)
  {
    switch (Phdr->p_type)
    {
    case PT_LOAD:
    {
      // round up
      INTN Pages = (Phdr->p_memsz + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
      Elf64_Addr Segment = Phdr->p_paddr;
      AllocatePage(AllocateAddress, EfiLoaderData, Pages, &Segment);

      Kernel->SetPosition(Kernel, Phdr->p_offset);
      UINTN Size = Phdr->p_filesz;
      Kernel->Read(Kernel, &Size, (VOID *)Segment);
      break;
    }
    default:
      break;
    }
  }
  FrameBuffer *newBuffer = InitGraphicsOutputProtocol(SystemTable);
  PSF1_FONT *newFont = LoadPSF1Font(NULL, gFontPath, ImageHandle, SystemTable);

  // Get the memory map from the Firmware
  EFI_MEMORY_DESCRIPTOR *Map = NULL;
  UINTN MapSize, MapKey;
  UINTN DescriptorSize;
  UINT32 DescriptorVersion;
  {
    EFI_GET_MEMORY_MAP GetMemoryMap = SystemTable->BootServices->GetMemoryMap;
    EFI_STATUS Status = GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status != EFI_BUFFER_TOO_SMALL)
    {
      ErrorPrint(L"GetMemoryMap Error");
      return Status;
    }
    AllocatePool(EfiLoaderData, MapSize, (VOID **)&Map);
    GetMemoryMap(&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
  }

  // Get the ACPI tables from the Firmware
  VOID *Rsdp = NULL;
  for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++)
  {
    EFI_CONFIGURATION_TABLE *Config = &SystemTable->ConfigurationTable[i];
    if (CompareMem(&Config->VendorGuid, &gEfiAcpiTableGuid, sizeof(Config->VendorGuid)) == 0)
    {
      Rsdp = Config->VendorTable;
      break;
    }
  }

  bootInfo.framebuffer = newBuffer;
  bootInfo.psf_1_font = newFont;
  bootInfo.mMap = Map;
  bootInfo.mMapSize = MapSize;
  bootInfo.mMapDescriptorSize = DescriptorSize;
  bootInfo.rsdp = Rsdp;

  // Jump to the kernel
  SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);
  typedef void EntryPoint(BootInfo *);
  ((EntryPoint *)Header.e_entry)(&bootInfo);
  return EFI_SUCCESS;
}
