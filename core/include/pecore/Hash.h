#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace PeCore
{
    enum class HashAlgorithm
    {
        HASH_MD5 = 0,
        HASH_SHA1 = 1,
        HASH_SHA256 = 2,
        HASH_BLAKE3 = 3,
        HASH_SHA384 = 4,
        HASH_SHA512 = 5
    };

    // Incremental hash over several pieces [the Authenticode digest skips parts of the file].
    class Hasher
    {
    private:

        struct Implementation;
        std::unique_ptr< Implementation > m_Implementation;

    public:

        explicit Hasher(
            HashAlgorithm algorithm
        );

        ~Hasher();

        Hasher(
            const Hasher &
        ) = delete;

        Hasher & operator=(
            const Hasher &
            ) = delete;

        void Update(
            std::span< const uint8_t > data
        );

        std::vector< uint8_t > Finish();
    };

    // Upper case hex digests of a whole file, computed together [one thread per algorithm].
    struct FileHashes
    {
        std::string m_Md5;
        std::string m_Sha1;
        std::string m_Sha256;
        std::string m_Blake3;
    };

    FileHashes ComputeFileHashes(
        std::span< const uint8_t > data
    );

    std::vector< uint8_t > ComputeHash(
        HashAlgorithm algorithm,
        std::span< const uint8_t > data
    );

    std::string HashToHex(
        const std::vector< uint8_t > & digest,
        bool upper_case
    );

    // Upper case hex digest.
    std::string ComputeHashHex(
        HashAlgorithm algorithm,
        std::span< const uint8_t > data
    );
}