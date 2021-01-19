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

  EFI_FILE *Kernel;
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

  // Jump to the kernel
  SystemTable->BootServices->ExitBootServices(ImageHandle, MapKey);
  typedef void EntryPoint(EFI_MEMORY_DESCRIPTOR *, UINTN, UINTN, VOID *);
  ((EntryPoint *)Header.e_entry)(Map, MapSize, DescriptorSize, Rsdp);
  return EFI_SUCCESS;
}
