#pragma once

#include "PeImage.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

/*
    Program database symbols. The PDB is read by an own MSF reader [portable, no DIA or DbgHelp)]. The info stream for the
    GUID, the DBI stream for the age, the module streams for function symbols [S_GPROC32, S_LPROC32 and the _ID forms]
    and the symbol record stream for public symbols [S_PUB32] and global data [S_GDATA32, S_LDATA32].
*/

namespace PeCore
{
    // RSDS CodeView record of the debug directory.
    struct CodeViewInfo
    {
        bool m_Present = false;
        std::string m_PdbPath;
        std::array< uint8_t, 16 > m_Guid = {};
        uint32_t m_Age = 0;

        // '{GUID}' for display.
        std::string GuidText() const;

        // GUID without dashes plus the age in hex, the folder name of a symbol store.
        std::string SearchKey() const;

        // File name part of 'm_PdbPath' ['pecore.pdb'].
        std::string PdbFileName() const;
    };

    CodeViewInfo ReadCodeView(
        const PeImage & image
    );

    struct Symbol
    {
        uint32_t m_Rva = 0;
        uint32_t m_Size = 0;
        std::string m_Name;
        bool m_Function = false;
    };

    class SymbolTable
    {
    private:

        std::vector< Symbol > m_Symbols;
        std::filesystem::path m_Source;
        uint32_t m_Publics = 0;
        uint32_t m_Functions = 0;

    public:

        // Reads the PDB and maps its symbols to RVAs of image. With 'require_match' a PDB of another build [GUID or age differ]
        // is refused, the error says why.
        static std::shared_ptr< SymbolTable > Load(
            const std::filesystem::path & path,
            const PeImage & image,
            const CodeViewInfo & code_view,
            bool require_match,
            std::string & error
        );

        const std::filesystem::path & GetSource() const
        {
            return m_Source;
        }

        size_t GetCount() const
        {
            return m_Symbols.size();
        }

        uint32_t GetFunctionCount() const
        {
            return m_Functions;
        }

        uint32_t GetPublicCount() const
        {
            return m_Publics;
        }

        // Closest symbol at or before rva, nullptr when there is none or rva is past a sized function.
        const Symbol * Find(
            uint32_t rva
        ) const;
    };

    // Names of addresses for the disassembler, the exception directory and the address converter.
    // PDB symbols when loaded, exports and import address table slots always.
    class AddressNames
    {
    private:

        std::shared_ptr< const SymbolTable > m_Symbols;
        std::map< uint32_t, std::string > m_Exports;
        std::map< uint32_t, std::string > m_ImportSlots;

    public:

        AddressNames(
            const PeImage & image,
            std::shared_ptr< const SymbolTable > symbols
        );

        // 'kernel32.dll!CreateFileW' for an IAT slot, 'Function+0x1C' inside a function, empty when unknown.
        // 'exact_only' returns names only for the first byte of a symbol [labels].
        std::string Describe(
            uint32_t rva,
            bool exact_only = false
        ) const;

        // Name of the import whose IAT slot is at rva, empty otherwise.
        std::string ImportSlot(
            uint32_t rva
        ) const;

        bool HasSymbols() const
        {
            return m_Symbols != nullptr;
        }
    };

    // Candidate files in lookup order [they may not exist], cache is the download folder of the application.
    std::vector< std::filesystem::path > PdbCandidates(
        const CodeViewInfo & code_view,
        const std::filesystem::path & image_path,
        const std::filesystem::path & cache
    );

    // URL of the PDB on a symbol server [https://msdl.microsoft.com/download/symbols by default].
    std::string SymbolServerUrl(
        const CodeViewInfo & code_view,
        const std::string & server
    );

    // Symbol store path of the PDB below cache [cache\name.pdb\KEY\name.pdb].
    std::filesystem::path SymbolStorePath(
        const CodeViewInfo & code_view,
        const std::filesystem::path & cache
    );
}