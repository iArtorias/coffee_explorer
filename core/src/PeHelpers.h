#pragma once

#include "pecore/PeImage.h"

#include "retdec/pelib/PeFile.h"

#include <string>
#include <vector>

// Internal helpers on top of the public PeLib API, shared by the view, tool and operation files.
namespace PeCore
{
    struct ImportFunction
    {
        std::string m_Name;
        uint16_t m_Hint = 0;
        uint32_t m_Ordinal = 0;
        uint32_t m_PatchRva = 0;
        bool m_ByOrdinal = false;
    };

    struct ImportModule
    {
        std::string m_Name;
        uint32_t m_OriginalFirstThunk = 0;
        uint32_t m_FirstThunk = 0;
        uint32_t m_TimeDateStamp = 0;
        uint32_t m_ForwarderChain = 0;
        uint32_t m_NameRva = 0;
        std::vector< ImportFunction > m_Functions;
    };

    inline std::vector< ImportModule > ReadImports(
        const PeImage & image
    )
    {
        std::vector< ImportModule > modules;

        if (!image.IsPe())
        {
            return modules;
        }

        const PeLib::ImportDirectory & imports = image.GetFile().impDir();
        uint32_t count = imports.getNumberOfFiles( false );
        modules.reserve( count );

        for (uint32_t file = 0; file < count; ++file)
        {
            ImportModule module;
            module.m_Name = imports.getFileName( file, false );
            module.m_OriginalFirstThunk = imports.getOriginalFirstThunk( file, false );
            module.m_FirstThunk = imports.getFirstThunk( file, false );
            module.m_TimeDateStamp = imports.getTimeDateStamp( file, false );
            module.m_ForwarderChain = imports.getForwarderChain( file, false );
            module.m_NameRva = imports.getRvaOfName( file, false );
            uint32_t functions = imports.getNumberOfFunctions( file, false );

            for (uint32_t index = 0; index < functions; ++index)
            {
                ImportFunction function;
                imports.getImportedFunction( file, index, function.m_Name, function.m_Hint, function.m_Ordinal, function.m_PatchRva, function.m_ByOrdinal, false );
                module.m_Functions.push_back( std::move( function ) );
            }

            modules.push_back( std::move( module ) );
        }

        return modules;
    }
}