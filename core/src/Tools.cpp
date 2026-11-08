#include "pecore/Tools.h"

#include "pecore/Hash.h"
#include "pecore/Views.h"

#include "PeHelpers.h"

#include "retdec/utils/ord_lookup.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef PECORE_WITH_ZYDIS
#include <Zydis/Zydis.h>
#endif

namespace PeCore
{
    std::string ConvertAddress(
        const PeImage & image,
        const std::string & kind,
        uint64_t value,
        const AddressNames * names
    )
    {
        Require( image.IsPe(), ErrorCode::ERR_NOT_PE, "The file is not a PE image" );

        uint64_t base = image.GetImageBase();
        uint64_t va = 0;
        uint32_t rva = 0;
        uint64_t offset = NO_OFFSET;
        bool valid = true;
        std::string message;

        if (kind == "va")
        {
            valid = value >= base && value - base <= 0xFFFFFFFFull;
            rva = valid ? static_cast<uint32_t>(value - base) : 0;
            va = value;
            offset = valid ? image.RvaToOffset( rva ) : NO_OFFSET;
            message = valid ? std::string() : "The VA is below the image base";
        }
        else if (kind == "rva")
        {
            valid = value <= 0xFFFFFFFFull;
            rva = static_cast<uint32_t>(value);
            va = base + rva;
            offset = valid ? image.RvaToOffset( rva ) : NO_OFFSET;
        }
        else if (kind == "offset")
        {
            offset = value;
            valid = image.OffsetToRva( value, rva );
            va = valid ? base + rva : 0;
            message = valid ? std::string() : value >= image.GetFileSize() ? "The offset is outside of the file" : "The offset is not mapped (overlay or slack space)";
        }
        else
        {
            throw Error( ErrorCode::ERR_INVALID_ARGUMENT, "Unknown address kind " + kind );
        }

        if (valid && offset == NO_OFFSET && message.empty())
        {
            message = rva < image.GetSizeOfImage() ? "The address is not backed by file data (uninitialized data)" : "The address is outside of the image";
        }

        JsonWriter writer;
        writer.BeginObject();
        writer.Member( "va", valid ? Hex( va, image.Is64() ? 16 : 8 ) : std::string() );
        writer.Member( "rva", valid ? Hex( rva ) : std::string() );
        writer.Member( "offset", offset != NO_OFFSET ? Hex( offset ) : std::string() );

        if (offset != NO_OFFSET)
        {
            writer.MemberUnsigned( "file_offset", offset );
        }

        writer.Member( "section", valid ? image.SectionNameOfRva( rva ) : std::string() );
        writer.Member( "symbol", valid && names != nullptr ? names->Describe( rva ) : std::string() );
        writer.MemberBool( "valid", valid && offset != NO_OFFSET );
        writer.Member( "message", message );
        writer.EndObject();
        return writer.TakeText();
    }

    std::string BuildImportSummary(
        const PeImage & image
    )
    {
        JsonWriter writer;
        writer.BeginObject();
        writer.MemberBool( "pe", image.IsPe() );
        writer.MemberBool( "is64", image.Is64() );
        writer.Key( "modules" ).BeginArray();

        for (const ImportModule & module : ReadImports( image ))
        {
            writer.BeginObject();
            writer.Member( "name", Utf8FromLatin1( module.m_Name ) );
            writer.MemberBool( "delay", false );
            writer.Key( "functions" ).BeginArray();

            for (const ImportFunction & function : module.m_Functions)
            {
                writer.BeginObject();
                writer.Member( "name", function.m_ByOrdinal ? std::string() : Utf8FromLatin1( function.m_Name ) );
                writer.MemberUnsigned( "ordinal", function.m_ByOrdinal ? function.m_Ordinal : 0 );
                writer.MemberUnsigned( "hint", function.m_Hint );
                writer.EndObject();
            }

            writer.EndArray();
            writer.EndObject();
        }

        if (image.IsPe())
        {
            const PeLib::DelayImportDirectory & delay = image.GetFile().delayImports();

            for (size_t index = 0; index < delay.getNumberOfFiles(); ++index)
            {
                const PeLib::PELIB_IMAGE_DELAY_IMPORT_DIRECTORY_RECORD * record = delay.getFile( index );
                writer.BeginObject();
                writer.Member( "name", Utf8FromLatin1( record->Name ) );
                writer.MemberBool( "delay", true );
                writer.Key( "functions" ).BeginArray();

                for (size_t function = 0; function < record->getNumberOfFunctions(); ++function)
                {
                    const PeLib::PELIB_DELAY_IMPORT * entry = record->getFunction( function );
                    writer.BeginObject();
                    writer.Member( "name", Utf8FromLatin1( entry->fname ) );
                    writer.MemberUnsigned( "ordinal", entry->fname.empty() ? entry->hint : 0 );
                    writer.MemberUnsigned( "hint", entry->hint );
                    writer.EndObject();
                }

                writer.EndArray();
                writer.EndObject();
            }
        }

        writer.EndArray();
        writer.EndObject();
        return writer.TakeText();
    }

    std::string BuildExportSummary(
        const PeImage & image
    )
    {
        JsonWriter writer;
        writer.BeginObject();
        writer.Key( "functions" ).BeginArray();

        if (image.IsPe() && image.HasDirectory( DIR_EXPORT ))
        {
            const PeLib::ExportDirectory & exports = image.GetFile().expDir();
            DataDirectory directory = image.GetDirectory( DIR_EXPORT );

            for (unsigned int index = 0; index < exports.calcNumberOfFunctions(); ++index)
            {
                uint32_t rva = exports.getAddressOfFunction( index );

                if (rva == 0)
                {
                    continue;
                }

                bool forwarder = rva >= directory.m_Rva && rva < static_cast<uint64_t>( directory.m_Rva ) + directory.m_Size;
                writer.BeginObject();
                writer.Member( "name", Utf8FromLatin1( exports.getFunctionName( index ) ) );
                writer.MemberUnsigned( "ordinal", exports.getFunctionOrdinal( index ) );
                writer.Member( "rva", Hex( rva ) );
                writer.Member( "forwarder", forwarder ? Utf8FromLatin1( image.ReadStringAtRva( rva, 512 ) ) : std::string() );
                writer.EndObject();
            }
        }

        writer.EndArray();

        if (image.IsPe() && image.HasDirectory( DIR_EXPORT ))
        {
            writer.Member( "name", Utf8FromLatin1( image.GetFile().expDir().getNameString() ) );
        }

        writer.EndObject();
        return writer.TakeText();
    }

    #ifdef PECORE_WITH_ZYDIS
    namespace
    {
        // Register token text back to the register [the tokenizer only hands out text].
        ZydisRegister FindRegister(
            std::string_view text
        )
        {
            static const std::unordered_map< std::string_view, ZydisRegister > registers = []
            {
                std::unordered_map< std::string_view, ZydisRegister > map;

                for (int value = ZYDIS_REGISTER_NONE + 1; value <= ZYDIS_REGISTER_MAX_VALUE; ++value)
                {
                    if (const char * name = ZydisRegisterGetString( static_cast<ZydisRegister>(value) ))
                    {
                        map.emplace( name, static_cast<ZydisRegister>(value) );
                    }
                }

                return map;
            }();

            auto found = registers.find( text );
            return found != registers.end() ? found->second : ZYDIS_REGISTER_NONE;
        }

        DisassemblyClass RegisterClass(
            ZydisRegister reg
        )
        {
            switch (ZydisRegisterGetClass( reg ))
            {
                case ZYDIS_REGCLASS_X87:
                    return DisassemblyClass::FPU_REGISTER;
                case ZYDIS_REGCLASS_MMX:
                    return DisassemblyClass::MMX_REGISTER;
                case ZYDIS_REGCLASS_XMM:
                    return DisassemblyClass::XMM_REGISTER;
                case ZYDIS_REGCLASS_YMM:
                    return DisassemblyClass::YMM_REGISTER;
                case ZYDIS_REGCLASS_ZMM:
                case ZYDIS_REGCLASS_TMM:
                    return DisassemblyClass::ZMM_REGISTER;
                case ZYDIS_REGCLASS_SEGMENT:
                    return DisassemblyClass::SEGMENT_REGISTER;
                case ZYDIS_REGCLASS_MASK:
                    return DisassemblyClass::MASK_REGISTER;
                default:
                    return DisassemblyClass::GENERAL_REGISTER;
            }
        }

        // The mnemonic color says what the instruction does to the control flow, like the x64dbg instruction classes.
        DisassemblyClass MnemonicClass(
            const ZydisDecodedInstruction & instruction
        )
        {
            switch (instruction.mnemonic)
            {
                case ZYDIS_MNEMONIC_PUSH:
                case ZYDIS_MNEMONIC_POP:
                case ZYDIS_MNEMONIC_PUSHA:
                case ZYDIS_MNEMONIC_PUSHAD:
                case ZYDIS_MNEMONIC_POPA:
                case ZYDIS_MNEMONIC_POPAD:
                case ZYDIS_MNEMONIC_PUSHF:
                case ZYDIS_MNEMONIC_PUSHFD:
                case ZYDIS_MNEMONIC_PUSHFQ:
                case ZYDIS_MNEMONIC_POPF:
                case ZYDIS_MNEMONIC_POPFD:
                case ZYDIS_MNEMONIC_POPFQ:
                    return DisassemblyClass::PUSH_POP;
                case ZYDIS_MNEMONIC_INT3:
                    return DisassemblyClass::INT3;
                case ZYDIS_MNEMONIC_NOP:
                case ZYDIS_MNEMONIC_FNOP:
                    return DisassemblyClass::NOP;
                case ZYDIS_MNEMONIC_INT:
                case ZYDIS_MNEMONIC_INT1:
                case ZYDIS_MNEMONIC_INTO:
                case ZYDIS_MNEMONIC_UD0:
                case ZYDIS_MNEMONIC_UD1:
                case ZYDIS_MNEMONIC_UD2:
                case ZYDIS_MNEMONIC_SYSCALL:
                case ZYDIS_MNEMONIC_SYSENTER:
                case ZYDIS_MNEMONIC_SYSEXIT:
                case ZYDIS_MNEMONIC_SYSRET:
                case ZYDIS_MNEMONIC_CPUID:
                case ZYDIS_MNEMONIC_RDTSC:
                case ZYDIS_MNEMONIC_RDTSCP:
                case ZYDIS_MNEMONIC_HLT:
                case ZYDIS_MNEMONIC_IN:
                case ZYDIS_MNEMONIC_OUT:
                    return DisassemblyClass::UNUSUAL;
                default:
                    break;
            }

            bool far_branch = instruction.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR;

            switch (instruction.meta.category)
            {
                case ZYDIS_CATEGORY_CALL:
                    return far_branch ? DisassemblyClass::FAR : DisassemblyClass::CALL;
                case ZYDIS_CATEGORY_RET:
                    return far_branch || instruction.mnemonic == ZYDIS_MNEMONIC_IRET || instruction.mnemonic == ZYDIS_MNEMONIC_IRETD ||
                        instruction.mnemonic == ZYDIS_MNEMONIC_IRETQ
                        ? DisassemblyClass::FAR
                        : DisassemblyClass::RET;
                case ZYDIS_CATEGORY_COND_BR:
                    return DisassemblyClass::CONDITIONAL_JUMP;
                case ZYDIS_CATEGORY_UNCOND_BR:
                    return far_branch ? DisassemblyClass::FAR : DisassemblyClass::UNCONDITIONAL_JUMP;
                case ZYDIS_CATEGORY_NOP:
                case ZYDIS_CATEGORY_WIDENOP:
                    return DisassemblyClass::NOP;
                case ZYDIS_CATEGORY_SYSCALL:
                case ZYDIS_CATEGORY_SYSRET:
                case ZYDIS_CATEGORY_INTERRUPT:
                case ZYDIS_CATEGORY_IO:
                case ZYDIS_CATEGORY_IOSTRINGOP:
                    return DisassemblyClass::UNUSUAL;
                default:
                    break;
            }

            // Ring 0 only [mov cr0, wrmsr, lgdt etc] stands out like in x64dbg.
            return (instruction.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) != 0 ? DisassemblyClass::UNUSUAL : DisassemblyClass::MNEMONIC;
        }

        // Formats one instruction through the tokenizer: the text plus [ class, length ] pairs that cover it exactly
        void FormatTokens(
            const ZydisFormatter & formatter,
            const ZydisDecodedInstruction & instruction,
            const ZydisDecodedOperand * operands,
            uint64_t address,
            std::string & text,
            std::vector< std::pair< DisassemblyClass, size_t > > & spans
        )
        {
            char buffer[512];
            const ZydisFormatterToken * token = nullptr;
            text.clear();
            spans.clear();

            if (!ZYAN_SUCCESS( ZydisFormatterTokenizeInstruction( &formatter, &instruction, operands, instruction.operand_count_visible, buffer,
                sizeof( buffer ), address, &token, ZYAN_NULL ) ))
            {
                // Never seen in practice, the plain formatter still gives a readable line.
                ZydisFormatterFormatInstruction( &formatter, &instruction, operands, instruction.operand_count_visible, buffer, sizeof( buffer ), address,
                    ZYAN_NULL );
                text = buffer;
                spans.emplace_back( DisassemblyClass::MNEMONIC, text.size() );
                return;
            }

            // Memory operand being formatted. Its base and index registers and whether it lives on the stack.
            const ZydisDecodedOperand * memory = nullptr;
            bool in_memory = false;
            bool after_scale_operator = false;
            DisassemblyClass mnemonic_class = MnemonicClass( instruction );

            auto next_memory = [&] ( const ZydisDecodedOperand * after ) -> const ZydisDecodedOperand *
            {
                for (uint8_t index = after == nullptr ? 0 : static_cast<uint8_t>(after - operands + 1); index < instruction.operand_count_visible; ++index)
                {
                    if (operands[index].type == ZYDIS_OPERAND_TYPE_MEMORY)
                    {
                        return &operands[index];
                    }
                }

                return nullptr;
            };

            for (; token != nullptr; )
            {
                ZydisTokenType type = ZYDIS_TOKEN_INVALID;
                ZyanConstCharPointer value = nullptr;

                if (!ZYAN_SUCCESS( ZydisFormatterTokenGetValue( token, &type, &value ) ) || value == nullptr)
                {
                    break;
                }

                std::string_view piece( value );
                DisassemblyClass kind = DisassemblyClass::TEXT;

                switch (type)
                {
                    case ZYDIS_TOKEN_MNEMONIC:
                        kind = mnemonic_class;
                        break;
                    case ZYDIS_TOKEN_PREFIX:
                        kind = DisassemblyClass::PREFIX;
                        break;
                    case ZYDIS_TOKEN_REGISTER:
                    {
                        ZydisRegister reg = FindRegister( piece );

                        if (in_memory && memory != nullptr && reg != ZYDIS_REGISTER_NONE && reg == memory->mem.base)
                        {
                            kind = DisassemblyClass::MEMORY_BASE_REGISTER;
                        }
                        else if (in_memory && memory != nullptr && reg != ZYDIS_REGISTER_NONE && reg == memory->mem.index)
                        {
                            kind = DisassemblyClass::MEMORY_INDEX_REGISTER;
                        }
                        else
                        {
                            kind = RegisterClass( reg );
                        }

                        break;
                    }
                    case ZYDIS_TOKEN_ADDRESS_ABS:
                    case ZYDIS_TOKEN_ADDRESS_REL:
                    case ZYDIS_TOKEN_SYMBOL:
                        kind = DisassemblyClass::ADDRESS;
                        break;
                    case ZYDIS_TOKEN_DISPLACEMENT:
                        kind = DisassemblyClass::VALUE;
                        break;
                    case ZYDIS_TOKEN_IMMEDIATE:
                        kind = in_memory && after_scale_operator ? DisassemblyClass::MEMORY_SCALE : DisassemblyClass::VALUE;
                        break;
                    case ZYDIS_TOKEN_TYPECAST:
                        kind = DisassemblyClass::MEMORY_SIZE;
                        break;
                    case ZYDIS_TOKEN_DECORATOR:
                        kind = DisassemblyClass::DECORATOR;
                        break;
                    case ZYDIS_TOKEN_PARENTHESIS_OPEN:
                        if (piece == "[")
                        {
                            memory = next_memory( memory );
                            in_memory = true;
                        }

                        [[fallthrough]];
                    case ZYDIS_TOKEN_PARENTHESIS_CLOSE:
                    {
                        bool stack = memory != nullptr && (memory->mem.segment == ZYDIS_REGISTER_SS || memory->mem.base == ZYDIS_REGISTER_RSP ||
                            memory->mem.base == ZYDIS_REGISTER_ESP || memory->mem.base == ZYDIS_REGISTER_SP ||
                            memory->mem.base == ZYDIS_REGISTER_RBP || memory->mem.base == ZYDIS_REGISTER_EBP ||
                            memory->mem.base == ZYDIS_REGISTER_BP);
                        kind = in_memory ? (stack ? DisassemblyClass::MEMORY_STACK_BRACKETS : DisassemblyClass::MEMORY_BRACKETS) : DisassemblyClass::TEXT;

                        if (type == ZYDIS_TOKEN_PARENTHESIS_CLOSE && piece == "]")
                        {
                            in_memory = false;
                            after_scale_operator = false;
                        }

                        break;
                    }
                    case ZYDIS_TOKEN_DELIMITER:
                        if (in_memory && (piece == "+" || piece == "-" || piece == "*"))
                        {
                            kind = DisassemblyClass::MEMORY_OPERATOR;
                            after_scale_operator = piece == "*";
                        }
                        else if (piece.find( ',' ) != std::string_view::npos)
                        {
                            kind = DisassemblyClass::COMMA;
                        }
                        else if (piece.find( ':' ) != std::string_view::npos)
                        {
                            kind = DisassemblyClass::MEMORY_OPERATOR;
                        }

                        break;
                    default:
                        break;
                }

                if (!piece.empty())
                {
                    text.append( piece );

                    // Neighbours of the same class share one span, the UI draws fewer runs.
                    if (!spans.empty() && spans.back().first == kind)
                    {
                        spans.back().second += piece.size();
                    }
                    else
                    {
                        spans.emplace_back( kind, piece.size() );
                    }
                }

                if (!ZYAN_SUCCESS( ZydisFormatterTokenNext( &token ) ))
                {
                    break;
                }
            }
        }
    }
    #endif

    bool IsDisassemblerAvailable()
    {
        #ifdef PECORE_WITH_ZYDIS
        return true;
        #else
        return false;
        #endif
    }

    std::string Disassemble(
        const PeImage & image,
        uint64_t offset,
        uint64_t size,
        uint64_t base_address,
        int bits,
        size_t max_lines,
        const AddressNames * names
    )
    {
        #ifdef PECORE_WITH_ZYDIS
        Require( bits == 16 || bits == 32 || bits == 64, ErrorCode::ERR_INVALID_ARGUMENT, "The disassembler supports 16, 32 and 64 bit code" );
        std::span< const uint8_t > code = image.Slice( offset, size );
        Require( !code.empty(), ErrorCode::ERR_OUT_OF_RANGE, "The offset is outside of the file" );

        ZydisDecoder decoder;
        ZydisMachineMode mode = bits == 64 ? ZYDIS_MACHINE_MODE_LONG_64 : bits == 32 ? ZYDIS_MACHINE_MODE_LEGACY_32 : ZYDIS_MACHINE_MODE_LEGACY_16;
        ZydisStackWidth stack = bits == 64 ? ZYDIS_STACK_WIDTH_64 : bits == 32 ? ZYDIS_STACK_WIDTH_32 : ZYDIS_STACK_WIDTH_16;
        Require( ZYAN_SUCCESS( ZydisDecoderInit( &decoder, mode, stack ) ), ErrorCode::ERR_INTERNAL, "Cannot initialize the decoder" );
        ZydisFormatter formatter;
        ZydisFormatterInit( &formatter, ZYDIS_FORMATTER_STYLE_INTEL );
        ZydisFormatterSetProperty( &formatter, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_FALSE );

        JsonWriter writer;
        writer.BeginObject();
        writer.Key( "lines" ).BeginArray();
        size_t position = 0;
        size_t lines = 0;

        std::string text;
        std::vector< std::pair< DisassemblyClass, size_t > > spans;

        // The listing uses the addresses of the loaded image when the base is the VA of the first byte.
        // Branch targets outside of the listing are then found through the sections, and the entry point is marked.
        uint32_t first_rva = 0;
        bool image_addresses = image.IsPe() && image.OffsetToRva( offset, first_rva ) && base_address == image.GetImageBase() + first_rva;
        uint64_t entry_address = image.IsPe() && image.GetEntryPoint() != 0 ? image.GetImageBase() + image.GetEntryPoint() : 0;

        // File offset of a branch target, 'NO_OFFSET' when it has no bytes in the file.
        auto target_offset = [&] ( uint64_t target ) -> uint64_t
        {
            if (target >= base_address && target - base_address < code.size())
            {
                return offset + (target - base_address);
            }

            if (image_addresses && target >= image.GetImageBase() && target - image.GetImageBase() < image.GetSizeOfImage())
            {
                return image.RvaToOffset( static_cast<uint32_t>(target - image.GetImageBase()) );
            }

            return NO_OFFSET;
        };

        while (position < code.size() && lines < max_lines)
        {
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            uint64_t address = base_address + position;
            size_t length = 1;

            std::string comment;

            // Direct branch [jcc, jmp, call, loop, jrcxz]. Its target, the UI follows it.
            const char * branch = nullptr;
            uint64_t branch_target = 0;

            if (ZYAN_SUCCESS( ZydisDecoderDecodeFull( &decoder, code.data() + position, code.size() - position, &instruction, operands ) ))
            {
                FormatTokens( formatter, instruction, operands, address, text, spans );
                length = instruction.length;

                bool is_call = instruction.meta.category == ZYDIS_CATEGORY_CALL;
                bool is_jump = instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
                bool is_conditional = instruction.meta.category == ZYDIS_CATEGORY_COND_BR;

                if ((is_call || is_jump || is_conditional) && instruction.operand_count_visible > 0 &&
                    operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[0].imm.is_relative)
                {
                    ZyanU64 target = 0;

                    if (ZYAN_SUCCESS( ZydisCalcAbsoluteAddress( &instruction, &operands[0], address, &target ) ))
                    {
                        branch = is_call ? "call" : is_jump ? "jmp" : "jcc";
                        branch_target = target;
                    }
                }

                // Branch targets and RIP relative memory 'call qword ptr [rip+...]' through the IAT names the import.
                for (uint8_t index = 0; names != nullptr && comment.empty() && index < instruction.operand_count_visible; ++index)
                {
                    const ZydisDecodedOperand & operand = operands[index];
                    bool relative = (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) ||
                        (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_RIP);
                    ZyanU64 target = 0;

                    if (!relative && operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE &&
                        operand.mem.index == ZYDIS_REGISTER_NONE && operand.mem.disp.has_displacement)
                    {
                        // 32 bit code calls imports through absolute addresses 'call dword ptr [0x401000]'.
                        target = static_cast<ZyanU64>(operand.mem.disp.value);
                    }
                    else if (!relative || !ZYAN_SUCCESS( ZydisCalcAbsoluteAddress( &instruction, &operand, address, &target ) ))
                    {
                        continue;
                    }

                    if (target >= image.GetImageBase() && target - image.GetImageBase() < image.GetSizeOfImage())
                    {
                        comment = names->Describe( static_cast<uint32_t>(target - image.GetImageBase()) );
                    }
                }
            }
            else
            {
                // Undecodable byte, the x64dbg 'unusual' color.
                text = "db " + Hex( code[position], 2 ) + "h";
                spans.assign( { { DisassemblyClass::UNUSUAL, 2 }, { DisassemblyClass::TEXT, 1 }, { DisassemblyClass::VALUE, text.size() - 3 } } );
            }

            std::string bytes;

            for (size_t byte = 0; byte < length; ++byte)
            {
                bytes += (byte == 0 ? "" : " ") + Hex( code[position + byte], 2 );
            }

            writer.BeginObject();
            writer.Member( "address", Hex( address, bits == 64 ? 16 : 8 ) );
            writer.MemberUnsigned( "offset", offset + position );
            writer.Member( "bytes", bytes );
            writer.Member( "text", text );

            if (names != nullptr && address >= image.GetImageBase() && address - image.GetImageBase() < image.GetSizeOfImage())
            {
                std::string label = names->Describe( static_cast<uint32_t>(address - image.GetImageBase()), true );

                if (!label.empty())
                {
                    writer.Member( "label", label );
                }
            }

            if (!comment.empty())
            {
                writer.Member( "comment", comment );
            }

            if (branch != nullptr)
            {
                writer.Member( "branch", branch );
                writer.Member( "target", Hex( branch_target, bits == 64 ? 16 : 8 ) );
                uint64_t target_file = target_offset( branch_target );

                if (target_file != NO_OFFSET)
                {
                    writer.MemberUnsigned( "target_offset", target_file );
                }
            }

            if (image_addresses && address == entry_address)
            {
                writer.MemberBool( "entry", true );
            }

            writer.Key( "spans" ).BeginArray();

            for (const auto & span : spans)
            {
                writer.Unsigned( static_cast<uint64_t>(span.first) );
                writer.Unsigned( span.second );
            }

            writer.EndArray();
            writer.EndObject();
            position += length;
            ++lines;
        }

        writer.EndArray();
        writer.MemberUnsigned( "next", offset + position );
        writer.EndObject();
        return writer.TakeText();
        #else
        throw Error( ErrorCode::ERR_UNSUPPORTED, "This build has no disassembler (configure with PECORE_WITH_ZYDIS)" );
        #endif
    }

    std::string BuildEntropyMap(
        const PeImage & image
    )
    {
        std::span< const uint8_t > data = image.GetData();
        uint64_t block = 256;

        while ((data.size() + block - 1) / block > ENTROPY_MAX_BLOCKS)
        {
            block *= 2;
        }

        JsonWriter writer;
        writer.BeginObject();
        writer.MemberUnsigned( "size", data.size() );
        writer.MemberUnsigned( "block", block );
        writer.Key( "values" ).BeginArray();

        for (uint64_t offset = 0; offset < data.size(); offset += block)
        {
            double entropy = ComputeEntropy( data.subspan( offset, std::min< uint64_t >( block, data.size() - offset ) ) );
            // Two decimals are plenty for a color and keep the answer small
            writer.Double( static_cast<double>( static_cast<int64_t>( entropy * 100.0 + 0.5 ) ) / 100.0 );
        }

        writer.EndArray();
        writer.Key( "regions" ).BeginArray();

        auto region = [&] ( const std::string & name, const char * kind, uint64_t start, uint64_t end )
        {
            if (end <= start || start >= data.size())
            {
                return;
            }

            writer.BeginObject();
            writer.Member( "name", name );
            writer.Member( "kind", kind );
            writer.MemberUnsigned( "start", start );
            writer.MemberUnsigned( "end", std::min< uint64_t >( end, data.size() ) );
            writer.EndObject();
        };

        if (image.IsPe())
        {
            region( "Headers", "headers", 0, std::min< uint64_t >( image.GetSizeOfHeaders(), data.size() ) );

            for (size_t index = 0; index < image.GetSectionCount(); ++index)
            {
                uint64_t header = image.GetSectionHeaderOffset( index );
                uint64_t raw_size = image.Read32( header + 16 );
                uint64_t raw_offset = image.Read32( header + 20 );
                region( image.GetSectionName( index ), "section", raw_offset, raw_offset + raw_size );
            }

            DataDirectory certificates = image.GetDirectory( DIR_SECURITY );

            if (certificates.m_Size != 0)
            {
                region( "Certificates", "certificates", certificates.m_Rva, static_cast<uint64_t>(certificates.m_Rva) + certificates.m_Size );
            }

            if (image.GetOverlaySize() != 0)
            {
                region( "Overlay", "overlay", image.GetOverlayOffset(), image.GetOverlayOffset() + image.GetOverlaySize() );
            }
        }

        writer.EndArray();
        writer.EndObject();
        return writer.TakeText();
    }
}