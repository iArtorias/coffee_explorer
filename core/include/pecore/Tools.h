#pragma once

#include "Json.h"
#include "PeImage.h"
#include "Symbols.h"

#include <string>

namespace PeCore
{
    // Address converter. kind is "va", "rva" or "offset". Answer { "va", "rva", "offset", "section", "valid", "message" }
    std::string ConvertAddress(
        const PeImage & image,
        const std::string & kind,
        uint64_t value,
        const AddressNames * names = nullptr
    );

    // { "modules": [ { "name", "delay", "functions": [ { "name", "ordinal", "hint" } ] } ] } for the dependency walker.
    std::string BuildImportSummary(
        const PeImage & image
    );

    // { "name", "functions": [ { "name", "ordinal", "rva", "forwarder" } ] } for the 'Import Adder'.
    std::string BuildExportSummary(
        const PeImage & image
    );

    bool IsDisassemblerAvailable();

    // { "lines": [ { "address", "offset", "bytes", "text" } ], "next" } , bits 16, 32 or 64.
    // 
    // Lines { "address", "offset", "bytes", "text", "label", "comment", "spans" }
    // 
    // Label names the address when a symbol starts there, comment names the target of a branch or of a RIP relative operand [imports through the IAT, PDB
    // symbols]. Spans colors the text like x64dbg [ class, length, class, length ... ] in UTF-8 bytes of 'text', the
    // classes are the 'DisassemblyClass' values below. Direct branches [jcc, jmp, call, loop] add 'branch' [jcc, jmp or
    // call], 'target' [address] and 'target_offset' [file offset, when the target has bytes in the file], and the line
    // at the entry point has 'entry'. True when the base is the address of the image.
    enum class DisassemblyClass : uint8_t
    {
        TEXT = 0, // Whitespace and anything without a color of its own.
        MNEMONIC,
        PUSH_POP,
        CALL,
        RET,
        CONDITIONAL_JUMP,
        UNCONDITIONAL_JUMP,
        NOP,
        FAR,
        INT3,
        UNUSUAL, // Privileged, system and undefined instructions, undecodable bytes.
        PREFIX,
        GENERAL_REGISTER,
        FPU_REGISTER,
        MMX_REGISTER,
        XMM_REGISTER,
        YMM_REGISTER,
        ZMM_REGISTER,
        SEGMENT_REGISTER,
        VALUE, // Immediates and displacements.
        ADDRESS, // Branch targets and absolute addresses.
        MEMORY_SIZE, // 'qword ptr'
        MEMORY_BRACKETS,
        MEMORY_STACK_BRACKETS, // Brackets of a memory operand based on the stack or frame pointer.
        MEMORY_BASE_REGISTER,
        MEMORY_INDEX_REGISTER,
        MEMORY_SCALE,
        MEMORY_OPERATOR, // '+' '-' '*' inside the brackets.
        COMMA,
        DECORATOR, // AVX-512 {k1} {z} {sae}
        MASK_REGISTER
    };

    std::string Disassemble(
        const PeImage & image,
        uint64_t offset,
        uint64_t size,
        uint64_t base_address,
        int bits,
        size_t max_lines,
        const AddressNames * names = nullptr
    );

    // Entropy map of the whole file for the overview strip:
    // { "size", "block", "values": [ bits per byte of each block ], "regions": [ { "name", "kind", "start", "end" } ] }
    // kind is headers, section, overlay or certificates. At most 'ENTROPY_MAX_BLOCKS' blocks of 256 bytes or more.
    constexpr size_t ENTROPY_MAX_BLOCKS = 2048;

    std::string BuildEntropyMap(
        const PeImage & image
    );
}