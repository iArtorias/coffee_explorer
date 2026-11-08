#include "pecore/Meta.h"

#include <array>

namespace PeCore
{
    namespace
    {
        const Choice MACHINES[] =
        {
            { 0x0000, "Unknown" },
            { 0x014C, "Intel 386" },
            { 0x0162, "MIPS R3000" },
            { 0x0166, "MIPS R4000" },
            { 0x0168, "MIPS R10000" },
            { 0x0169, "MIPS WCE v2" },
            { 0x0184, "Alpha AXP" },
            { 0x01A2, "Hitachi SH3" },
            { 0x01A3, "Hitachi SH3 DSP" },
            { 0x01A6, "Hitachi SH4" },
            { 0x01A8, "Hitachi SH5" },
            { 0x01C0, "ARM" },
            { 0x01C2, "ARM Thumb" },
            { 0x01C4, "ARM Thumb-2 (ARMNT)" },
            { 0x01D3, "Matsushita AM33" },
            { 0x01F0, "PowerPC" },
            { 0x01F1, "PowerPC FP" },
            { 0x0200, "Intel Itanium (IA64)" },
            { 0x0266, "MIPS16" },
            { 0x0284, "Alpha AXP 64" },
            { 0x0366, "MIPS FPU" },
            { 0x0466, "MIPS16 FPU" },
            { 0x0520, "Infineon TriCore" },
            { 0x0EBC, "EFI Byte Code" },
            { 0x5032, "RISC-V 32" },
            { 0x5064, "RISC-V 64" },
            { 0x5128, "RISC-V 128" },
            { 0x6232, "LoongArch 32" },
            { 0x6264, "LoongArch 64" },
            { 0x8664, "AMD64 (x64)" },
            { 0x9041, "Mitsubishi M32R" },
            { 0xA641, "ARM64EC" },
            { 0xA64E, "ARM64X" },
            { 0xAA64, "ARM64" },
            { 0xC0EE, "CLR pure MSIL" }
        };

        const Choice FILE_CHARACTERISTICS[] =
        {
            { 0x0001, "Relocation info stripped from file" },
            { 0x0002, "File is executable" },
            { 0x0004, "Line numbers stripped from file" },
            { 0x0008, "Local symbols stripped from file" },
            { 0x0010, "Aggressively trim working set (obsolete)" },
            { 0x0020, "App can handle >2 GB address space" },
            { 0x0040, "Reserved (0x0040)" },
            { 0x0080, "Bytes of machine word are reversed, low (obsolete)" },
            { 0x0100, "32 bit word machine" },
            { 0x0200, "Debugging info stripped from file in .DBG file" },
            { 0x0400, "If image is on removable media, copy and run from the swap file" },
            { 0x0800, "If image is on network, copy and run from the swap file" },
            { 0x1000, "System file" },
            { 0x2000, "File is a DLL" },
            { 0x4000, "File should only be run on a UP machine" },
            { 0x8000, "Bytes of machine word are reversed, high (obsolete)" }
        };

        const Choice SUBSYSTEMS[] =
        {
            { 0, "Unknown" },
            { 1, "Native" },
            { 2, "Windows GUI" },
            { 3, "Windows console" },
            { 5, "OS/2 console" },
            { 7, "POSIX console" },
            { 8, "Native Windows 9x driver" },
            { 9, "Windows CE GUI" },
            { 10, "EFI application" },
            { 11, "EFI boot service driver" },
            { 12, "EFI runtime driver" },
            { 13, "EFI ROM" },
            { 14, "Xbox" },
            { 16, "Windows boot application" }
        };

        const Choice DLL_CHARACTERISTICS[] =
        {
            { 0x0001, "Process init (reserved)" },
            { 0x0002, "Process term (reserved)" },
            { 0x0004, "Thread init (reserved)" },
            { 0x0008, "Thread term (reserved)" },
            { 0x0020, "High entropy 64 bit address space (HIGH_ENTROPY_VA)" },
            { 0x0040, "DLL can move (DYNAMIC_BASE, ASLR)" },
            { 0x0080, "Code integrity checks are enforced (FORCE_INTEGRITY)" },
            { 0x0100, "Image is NX compatible (NX_COMPAT, DEP)" },
            { 0x0200, "Isolation aware, do not isolate (NO_ISOLATION)" },
            { 0x0400, "Image does not use SEH (NO_SEH)" },
            { 0x0800, "Do not bind the image (NO_BIND)" },
            { 0x1000, "Image must execute in an AppContainer (APPCONTAINER)" },
            { 0x2000, "WDM driver (WDM_DRIVER)" },
            { 0x4000, "Image supports Control Flow Guard (GUARD_CF)" },
            { 0x8000, "Terminal Server aware (TERMINAL_SERVER_AWARE)" }
        };

        const Choice SECTION_CHARACTERISTICS[] =
        {
            { 0x00000008, "No padding (TYPE_NO_PAD, obsolete)" },
            { 0x00000020, "Contains code (CNT_CODE)" },
            { 0x00000040, "Contains initialized data (CNT_INITIALIZED_DATA)" },
            { 0x00000080, "Contains uninitialized data (CNT_UNINITIALIZED_DATA)" },
            { 0x00000100, "Link other (LNK_OTHER, reserved)" },
            { 0x00000200, "Contains comments or other info (LNK_INFO)" },
            { 0x00000800, "Will not become part of the image (LNK_REMOVE)" },
            { 0x00001000, "Contains COMDAT data (LNK_COMDAT)" },
            { 0x00004000, "Reset speculative exceptions (NO_DEFER_SPEC_EXC)" },
            { 0x00008000, "Referenced through the global pointer (GPREL)" },
            { 0x00020000, "Purgeable or 16 bit (MEM_PURGEABLE)" },
            { 0x00040000, "Locked (MEM_LOCKED)" },
            { 0x00080000, "Preload (MEM_PRELOAD)" },
            { 0x01000000, "Contains extended relocations (LNK_NRELOC_OVFL)" },
            { 0x02000000, "Can be discarded (MEM_DISCARDABLE)" },
            { 0x04000000, "Cannot be cached (MEM_NOT_CACHED)" },
            { 0x08000000, "Is not pageable (MEM_NOT_PAGED)" },
            { 0x10000000, "Is shareable (MEM_SHARED)" },
            { 0x20000000, "Is executable (MEM_EXECUTE)" },
            { 0x40000000, "Is readable (MEM_READ)" },
            { 0x80000000, "Is writeable (MEM_WRITE)" }
        };

        const Choice OPTIONAL_MAGIC[] =
        {
            { 0x010B, "PE32" },
            { 0x020B, "PE32+ (PE64)" },
            { 0x0107, "ROM image" }
        };

        const Choice DEBUG_TYPES[] =
        {
            { 0, "Unknown" },
            { 1, "COFF" },
            { 2, "CodeView" },
            { 3, "FPO" },
            { 4, "Misc" },
            { 5, "Exception" },
            { 6, "Fixup" },
            { 7, "OMAP to source" },
            { 8, "OMAP from source" },
            { 9, "Borland" },
            { 10, "Reserved10" },
            { 11, "CLSID" },
            { 12, "VC feature" },
            { 13, "POGO (profile guided optimization)" },
            { 14, "ILTCG" },
            { 15, "MPX" },
            { 16, "Repro (deterministic build)" },
            { 17, "Embedded portable PDB" },
            { 19, "PDB checksum" },
            { 20, "Extended DLL characteristics" }
        };

        const Choice DEBUG_EX_CHARACTERISTICS[] =
        {
            { 0x01, "CET compatible (CET_COMPAT)" },
            { 0x02, "CET compatible in strict mode only" },
            { 0x04, "CET set context IP validation relaxed" },
            { 0x08, "CET dynamic APIs allowed in process" },
            { 0x40, "Forward CFI compatible" },
            { 0x80, "Hotpatch compatible" }
        };

        const Choice RELOCATION_TYPES[] =
        {
            { 0, "ABSOLUTE" },
            { 1, "HIGH" },
            { 2, "LOW" },
            { 3, "HIGHLOW" },
            { 4, "HIGHADJ" },
            { 5, "MACHINE_SPECIFIC_5 (MIPS_JMPADDR, ARM_MOV32 or RISCV_HIGH20)" },
            { 7, "MACHINE_SPECIFIC_7 (THUMB_MOV32 or RISCV_LOW12I)" },
            { 8, "MACHINE_SPECIFIC_8 (RISCV_LOW12S or LOONGARCH_MARK_LA)" },
            { 9, "MACHINE_SPECIFIC_9 (MIPS_JMPADDR16)" },
            { 10, "DIR64" }
        };

        const Choice COR20_FLAGS[] =
        {
            { 0x00000001, "IL only (ILONLY)" },
            { 0x00000002, "32 bit required (32BITREQUIRED)" },
            { 0x00000004, "IL library (ILLIBRARY)" },
            { 0x00000008, "Strong name signed (STRONGNAMESIGNED)" },
            { 0x00000010, "Native entry point (NATIVE_ENTRYPOINT)" },
            { 0x00010000, "Track debug data (TRACKDEBUGDATA)" },
            { 0x00020000, "32 bit preferred (32BITPREFERRED)" }
        };

        const Choice GUARD_FLAGS[] =
        {
            { 0x00000100, "CF instrumented" },
            { 0x00000200, "CFW instrumented" },
            { 0x00000400, "CF function table present" },
            { 0x00000800, "Security cookie unused" },
            { 0x00001000, "Protect delay load IAT" },
            { 0x00002000, "Delay load IAT in its own section" },
            { 0x00004000, "CF export suppression info present" },
            { 0x00008000, "CF enable export suppression" },
            { 0x00010000, "CF longjump table present" },
            { 0x00020000, "RF instrumented" },
            { 0x00040000, "RF enable" },
            { 0x00080000, "RF strict" },
            { 0x00100000, "Retpoline present" },
            { 0x00400000, "EH continuation table present" },
            { 0x00800000, "XFG enabled" },
            { 0x01000000, "CastGuard present" },
            { 0x02000000, "Memcpy present" }
        };

        const Choice CERTIFICATE_TYPES[] =
        {
            { 1, "X.509" },
            { 2, "PKCS #7 signed data (Authenticode)" },
            { 3, "Reserved" },
            { 4, "Terminal server protocol stack" }
        };

        const Choice CERTIFICATE_REVISIONS[] =
        {
            { 0x0100, "1.0" },
            { 0x0200, "2.0" }
        };

        const Choice UNWIND_FLAGS[] =
        {
            { 0x01, "Exception handler (UNW_FLAG_EHANDLER)" },
            { 0x02, "Termination handler (UNW_FLAG_UHANDLER)" },
            { 0x04, "Chained (UNW_FLAG_CHAININFO)" }
        };

        const Choice TYPE_ATTRIBUTES[] =
        {
            { 0x00000001, "Public", 0x00000007 },
            { 0x00000002, "Nested public", 0x00000007 },
            { 0x00000003, "Nested private", 0x00000007 },
            { 0x00000004, "Nested family", 0x00000007 },
            { 0x00000005, "Nested assembly", 0x00000007 },
            { 0x00000006, "Nested family and assembly", 0x00000007 },
            { 0x00000007, "Nested family or assembly", 0x00000007 },
            { 0x00000008, "Sequential layout", 0x00000018 },
            { 0x00000010, "Explicit layout", 0x00000018 },
            { 0x00000020, "Interface" },
            { 0x00000080, "Abstract" },
            { 0x00000100, "Sealed" },
            { 0x00000400, "Special name" },
            { 0x00000800, "RT special name" },
            { 0x00001000, "Import" },
            { 0x00002000, "Serializable" },
            { 0x00004000, "Windows Runtime" },
            { 0x00010000, "Unicode class", 0x00030000 },
            { 0x00020000, "Auto class", 0x00030000 },
            { 0x00040000, "Custom format class" },
            { 0x00080000, "Before field init" },
            { 0x00100000, "Has security" },
            { 0x00200000, "Forwarder" }
        };

        const Choice METHOD_ATTRIBUTES[] =
        {
            { 0x0001, "Private", 0x0007 },
            { 0x0002, "Family and assembly", 0x0007 },
            { 0x0003, "Assembly", 0x0007 },
            { 0x0004, "Family", 0x0007 },
            { 0x0005, "Family or assembly", 0x0007 },
            { 0x0006, "Public", 0x0007 },
            { 0x0010, "Static" },
            { 0x0020, "Final" },
            { 0x0040, "Virtual" },
            { 0x0080, "Hide by signature" },
            { 0x0100, "New slot" },
            { 0x0200, "Strict" },
            { 0x0400, "Abstract" },
            { 0x0800, "Special name" },
            { 0x1000, "RT special name" },
            { 0x2000, "P/Invoke implementation" },
            { 0x4000, "Has security" },
            { 0x8000, "Require security object" }
        };

        const Choice FIELD_ATTRIBUTES[] =
        {
            { 0x0001, "Private", 0x0007 },
            { 0x0002, "Family and assembly", 0x0007 },
            { 0x0003, "Assembly", 0x0007 },
            { 0x0004, "Family", 0x0007 },
            { 0x0005, "Family or assembly", 0x0007 },
            { 0x0006, "Public", 0x0007 },
            { 0x0010, "Static" },
            { 0x0020, "Init only" },
            { 0x0040, "Literal" },
            { 0x0080, "Not serialized" },
            { 0x0100, "Has field RVA" },
            { 0x0200, "Special name" },
            { 0x0400, "RT special name" },
            { 0x1000, "Has field marshal" },
            { 0x2000, "P/Invoke implementation" },
            { 0x8000, "Has default" }
        };

        const Choice RESOURCE_TYPES[] =
        {
            { 1, "Cursors" },
            { 2, "Bitmaps" },
            { 3, "Icons" },
            { 4, "Menus" },
            { 5, "Dialogs" },
            { 6, "String Tables" },
            { 7, "Font Directory" },
            { 8, "Fonts" },
            { 9, "Accelerators" },
            { 10, "RCData" },
            { 11, "Message Tables" },
            { 12, "Cursor Groups" },
            { 14, "Icon Groups" },
            { 16, "Version Info" },
            { 17, "Dialog Include" },
            { 19, "Plug and Play" },
            { 20, "VXD" },
            { 21, "Animated Cursors" },
            { 22, "Animated Icons" },
            { 23, "HTML" },
            { 24, "Manifests" },
            { 240, "DLG Init" },
            { 241, "Toolbars" }
        };

        const ChoiceSet CHOICE_SETS[] =
        {
            { "machine", false, MACHINES },
            { "file_characteristics", true, FILE_CHARACTERISTICS },
            { "subsystem", false, SUBSYSTEMS },
            { "dll_characteristics", true, DLL_CHARACTERISTICS },
            { "section_characteristics", true, SECTION_CHARACTERISTICS },
            { "optional_magic", false, OPTIONAL_MAGIC },
            { "debug_type", false, DEBUG_TYPES },
            { "debug_ex_characteristics", true, DEBUG_EX_CHARACTERISTICS },
            { "relocation_type", false, RELOCATION_TYPES },
            { "cor20_flags", true, COR20_FLAGS },
            { "guard_flags", true, GUARD_FLAGS },
            { "certificate_type", false, CERTIFICATE_TYPES },
            { "certificate_revision", false, CERTIFICATE_REVISIONS },
            { "unwind_flags", true, UNWIND_FLAGS },
            { "type_attributes", true, TYPE_ATTRIBUTES },
            { "method_attributes", true, METHOD_ATTRIBUTES },
            { "field_attributes", true, FIELD_ATTRIBUTES },
            { "resource_type", false, RESOURCE_TYPES }
        };

        struct LanguageEntry
        {
            uint16_t m_Id;
            const char * m_Name;
        };

        const LanguageEntry LANGUAGES[] =
        {
            { 0x0000, "Neutral" },
            { 0x0400, "Process default" },
            { 0x0800, "System default" },
            { 0x0401, "Arabic (Saudi Arabia)" },
            { 0x0402, "Bulgarian" },
            { 0x0403, "Catalan" },
            { 0x0404, "Chinese (Taiwan)" },
            { 0x0405, "Czech" },
            { 0x0406, "Danish" },
            { 0x0407, "German (Germany)" },
            { 0x0408, "Greek" },
            { 0x0409, "English (United States)" },
            { 0x040A, "Spanish (Traditional sort)" },
            { 0x040B, "Finnish" },
            { 0x040C, "French (France)" },
            { 0x040D, "Hebrew" },
            { 0x040E, "Hungarian" },
            { 0x0410, "Italian (Italy)" },
            { 0x0411, "Japanese" },
            { 0x0412, "Korean" },
            { 0x0413, "Dutch (Netherlands)" },
            { 0x0414, "Norwegian (Bokmal)" },
            { 0x0415, "Polish" },
            { 0x0416, "Portuguese (Brazil)" },
            { 0x0418, "Romanian" },
            { 0x0419, "Russian" },
            { 0x041A, "Croatian" },
            { 0x041B, "Slovak" },
            { 0x041D, "Swedish" },
            { 0x041E, "Thai" },
            { 0x041F, "Turkish" },
            { 0x0422, "Ukrainian" },
            { 0x0424, "Slovenian" },
            { 0x0425, "Estonian" },
            { 0x0426, "Latvian" },
            { 0x0427, "Lithuanian" },
            { 0x042A, "Vietnamese" },
            { 0x0804, "Chinese (Simplified)" },
            { 0x0807, "German (Switzerland)" },
            { 0x0809, "English (United Kingdom)" },
            { 0x080A, "Spanish (Mexico)" },
            { 0x0816, "Portuguese (Portugal)" },
            { 0x0C0A, "Spanish (Spain)" },
            { 0x0C0C, "French (Canada)" }
        };
    }

    const ChoiceSet * FindChoiceSet(
        std::string_view id
    )
    {
        for (const ChoiceSet & set : CHOICE_SETS)
        {
            if (id == set.m_Id)
            {
                return &set;
            }
        }

        return nullptr;
    }

    std::string DescribeEnum(
        std::string_view id,
        uint64_t value
    )
    {
        const ChoiceSet * set = FindChoiceSet( id );

        if (set == nullptr)
        {
            return std::string();
        }

        for (const Choice & choice : set->m_Items)
        {
            if (choice.m_Value == value)
            {
                return choice.m_Name;
            }
        }

        return std::string();
    }

    std::string DescribeFlags(
        std::string_view id,
        uint64_t value
    )
    {
        const ChoiceSet * set = FindChoiceSet( id );

        if (set == nullptr || value == 0)
        {
            return std::string();
        }

        std::string text;
        uint64_t known = 0;

        for (const Choice & choice : set->m_Items)
        {
            uint64_t mask = choice.m_Mask != 0 ? choice.m_Mask : choice.m_Value;

            if (mask != 0 && (value & mask) == choice.m_Value && (choice.m_Value != 0 || choice.m_Mask != 0))
            {
                if (!text.empty())
                {
                    text += ", ";
                }

                // Short form. The part in parentheses when there is one, so the 'Meaning' column stays readable.
                std::string name = choice.m_Name;
                size_t open = name.rfind( '(' );
                size_t close = name.rfind( ')' );

                if (open != std::string::npos && close == name.size() - 1)
                {
                    std::string inner = name.substr( open + 1, close - open - 1 );
                    inner = inner.substr( 0, inner.find( ',' ) );
                    bool constant = !inner.empty();

                    for (char character : inner)
                    {
                        if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_'))
                        {
                            constant = false;
                        }
                    }

                    if (constant)
                    {
                        name = inner;
                    }
                }

                text += name;
                known |= mask;
            }
        }

        uint64_t unknown = value & ~known;

        if (unknown != 0)
        {
            if (!text.empty())
            {
                text += ", ";
            }

            text += Hex( unknown, 0 ).insert( 0, "0x" );
        }

        return text;
    }

    void WriteChoiceSet(
        JsonWriter & writer,
        const ChoiceSet & set
    )
    {
        writer.BeginObject();
        writer.MemberBool( "flags", set.m_Flags );
        writer.Key( "items" ).BeginArray();

        for (const Choice & choice : set.m_Items)
        {
            writer.BeginObject();
            writer.Member( "value", Hex( choice.m_Value, 1 ) );
            writer.Member( "name", choice.m_Name );

            if (choice.m_Mask != 0)
            {
                writer.Member( "mask", Hex( choice.m_Mask, 1 ) );
            }

            writer.EndObject();
        }

        writer.EndArray();
        writer.EndObject();
    }

    std::string DirectoryName(
        uint32_t index
    )
    {
        static const char * NAMES[] =
        {
            "Export Directory", "Import Directory", "Resource Directory", "Exception Directory", "Security Directory",
            "Relocation Directory", "Debug Directory", "Architecture Directory", "Global Pointer", "TLS Directory",
            "Load Config Directory", "Bound Import Directory", "Import Address Table", "Delay Import Directory",
            ".NET Directory", "Reserved"
        };

        return index < 16 ? NAMES[index] : "Directory " + std::to_string( index );
    }

    std::string ResourceTypeName(
        uint32_t id
    )
    {
        std::string name = DescribeEnum( "resource_type", id );
        return name.empty() ? std::to_string( id ) : name;
    }

    std::string LanguageName(
        uint32_t language
    )
    {
        for (const LanguageEntry & entry : LANGUAGES)
        {
            if (entry.m_Id == language)
            {
                return entry.m_Name;
            }
        }

        return "Language " + Hex( language, 4 );
    }
}