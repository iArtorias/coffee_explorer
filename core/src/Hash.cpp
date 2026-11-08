#include "pecore/Hash.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <memory>

#include "blake3.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

// On Windows the system provider [CNG, SHA instructions when the CPU has them] does the MD5 and SHA work, several times
// faster on large files. The portable code stays as the fallback and for the other platforms.
namespace PeCore
{
    namespace
    {
        // Not 'RotateLeft32' and 'RotateRight32', since the MinGW's 'winnt.h' defines macros with those names.
        uint32_t Rotl32(
            uint32_t value,
            int count
        )
        {
            return (value << count) | (value >> (32 - count));
        }

        uint32_t Rotr32(
            uint32_t value,
            int count
        )
        {
            return (value >> count) | (value << (32 - count));
        }

        template< size_t BLOCK >
        class BlockHasher
        {
        private:

            uint8_t m_Buffer[BLOCK] = {};
            size_t m_BufferLength = 0;

        protected:

            uint64_t m_TotalLength = 0;

            virtual void Transform(
                const uint8_t * block
            ) = 0;

            // Appends the padding, length_bytes is 8 for MD5, SHA-1 and SHA-256 and 16 for SHA-512.
            void Pad(
                bool big_endian,
                size_t length_bytes
            )
            {
                uint64_t bit_length = m_TotalLength * 8;
                uint8_t marker = 0x80;
                Update( &marker, 1 );

                uint8_t zero = 0;

                while (m_BufferLength != BLOCK - length_bytes)
                {
                    Update( &zero, 1 );
                }

                uint8_t length[16] = {};

                for (size_t index = 0; index < 8; ++index)
                {
                    uint8_t byte = static_cast<uint8_t>( bit_length >> (index * 8) );
                    length[big_endian ? length_bytes - 1 - index : index] = byte;
                }

                Update( length, length_bytes );
            }

        public:

            void Update(
                const uint8_t * data,
                size_t length
            )
            {
                m_TotalLength += length;

                while (length > 0)
                {
                    if (m_BufferLength == 0 && length >= BLOCK)
                    {
                        Transform( data );
                        data += BLOCK;
                        length -= BLOCK;
                        continue;
                    }

                    size_t chunk = std::min( length, BLOCK - m_BufferLength );
                    std::memcpy( m_Buffer + m_BufferLength, data, chunk );
                    m_BufferLength += chunk;
                    data += chunk;
                    length -= chunk;

                    if (m_BufferLength == BLOCK)
                    {
                        Transform( m_Buffer );
                        m_BufferLength = 0;
                    }
                }
            }
        };

        class Md5Hasher : public BlockHasher< 64 >
        {
        private:

            uint32_t m_State[4] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };

            void Transform(
                const uint8_t * block
            )
            {
                static const uint32_t TABLE[64] =
                {
                    0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
                    0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
                    0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
                    0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED, 0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
                    0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
                    0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
                    0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
                    0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391
                };

                static const int SHIFTS[64] =
                {
                    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
                };

                uint32_t words[16];

                for (int index = 0; index < 16; ++index)
                {
                    words[index] = static_cast<uint32_t>( block[index * 4] ) | static_cast<uint32_t>( block[index * 4 + 1] ) << 8 |
                        static_cast<uint32_t>( block[index * 4 + 2] ) << 16 | static_cast<uint32_t>( block[index * 4 + 3] ) << 24;
                }

                uint32_t a = m_State[0];
                uint32_t b = m_State[1];
                uint32_t c = m_State[2];
                uint32_t d = m_State[3];

                for (int round = 0; round < 64; ++round)
                {
                    uint32_t f = 0;
                    int word = 0;

                    if (round < 16)
                    {
                        f = (b & c) | (~b & d);
                        word = round;
                    }
                    else if (round < 32)
                    {
                        f = (d & b) | (~d & c);
                        word = (5 * round + 1) % 16;
                    }
                    else if (round < 48)
                    {
                        f = b ^ c ^ d;
                        word = (3 * round + 5) % 16;
                    }
                    else
                    {
                        f = c ^ (b | ~d);
                        word = (7 * round) % 16;
                    }

                    uint32_t temp = d;
                    d = c;
                    c = b;
                    b = b + Rotl32( a + f + TABLE[round] + words[word], SHIFTS[round] );
                    a = temp;
                }

                m_State[0] += a;
                m_State[1] += b;
                m_State[2] += c;
                m_State[3] += d;
            }

        public:
            std::vector< uint8_t > Finish()
            {
                Pad( false, 8 );
                std::vector< uint8_t > out( 16 );

                for (int index = 0; index < 16; ++index)
                {
                    out[index] = static_cast<uint8_t>( m_State[index / 4] >> ((index % 4) * 8) );
                }

                return out;
            }
        };

        class Sha1Hasher : public BlockHasher< 64 >
        {
        private:

            uint32_t m_State[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

            void Transform(
                const uint8_t * block
            )
            {
                uint32_t words[80];

                for (int index = 0; index < 16; ++index)
                {
                    words[index] = static_cast<uint32_t>( block[index * 4] ) << 24 | static_cast<uint32_t>( block[index * 4 + 1] ) << 16 |
                        static_cast<uint32_t>( block[index * 4 + 2] ) << 8 | static_cast<uint32_t>( block[index * 4 + 3] );
                }

                for (int index = 16; index < 80; ++index)
                {
                    words[index] = Rotl32( words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1 );
                }

                uint32_t a = m_State[0];
                uint32_t b = m_State[1];
                uint32_t c = m_State[2];
                uint32_t d = m_State[3];
                uint32_t e = m_State[4];

                for (int round = 0; round < 80; ++round)
                {
                    uint32_t f = 0;
                    uint32_t k = 0;

                    if (round < 20)
                    {
                        f = (b & c) | (~b & d);
                        k = 0x5A827999;
                    }
                    else if (round < 40)
                    {
                        f = b ^ c ^ d;
                        k = 0x6ED9EBA1;
                    }
                    else if (round < 60)
                    {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8F1BBCDC;
                    }
                    else
                    {
                        f = b ^ c ^ d;
                        k = 0xCA62C1D6;
                    }

                    uint32_t temp = Rotl32( a, 5 ) + f + e + k + words[round];
                    e = d;
                    d = c;
                    c = Rotl32( b, 30 );
                    b = a;
                    a = temp;
                }

                m_State[0] += a;
                m_State[1] += b;
                m_State[2] += c;
                m_State[3] += d;
                m_State[4] += e;
            }

        public:

            std::vector< uint8_t > Finish()
            {
                Pad( true, 8 );
                std::vector< uint8_t > out( 20 );

                for (int index = 0; index < 20; ++index)
                {
                    out[index] = static_cast<uint8_t>( m_State[index / 4] >> (24 - (index % 4) * 8) );
                }

                return out;
            }
        };

        class Sha256Hasher : public BlockHasher< 64 >
        {
        private:

            uint32_t m_State[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };

            void Transform(
                const uint8_t * block
            )
            {
                static const uint32_t TABLE[64] =
                {
                    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
                    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
                    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
                    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
                    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
                    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
                    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
                    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
                };

                uint32_t words[64];

                for (int index = 0; index < 16; ++index)
                {
                    words[index] = static_cast<uint32_t>( block[index * 4] ) << 24 | static_cast<uint32_t>( block[index * 4 + 1] ) << 16 |
                        static_cast<uint32_t>( block[index * 4 + 2] ) << 8 | static_cast<uint32_t>( block[index * 4 + 3] );
                }

                for (int index = 16; index < 64; ++index)
                {
                    uint32_t s0 = Rotr32( words[index - 15], 7 ) ^ Rotr32( words[index - 15], 18 ) ^ (words[index - 15] >> 3);
                    uint32_t s1 = Rotr32( words[index - 2], 17 ) ^ Rotr32( words[index - 2], 19 ) ^ (words[index - 2] >> 10);
                    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
                }

                uint32_t state[8];
                std::memcpy( state, m_State, sizeof( state ) );

                for (int round = 0; round < 64; ++round)
                {
                    uint32_t s1 = Rotr32( state[4], 6 ) ^ Rotr32( state[4], 11 ) ^ Rotr32( state[4], 25 );
                    uint32_t choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
                    uint32_t temp1 = state[7] + s1 + choice + TABLE[round] + words[round];
                    uint32_t s0 = Rotr32( state[0], 2 ) ^ Rotr32( state[0], 13 ) ^ Rotr32( state[0], 22 );
                    uint32_t majority = (state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]);
                    uint32_t temp2 = s0 + majority;

                    state[7] = state[6];
                    state[6] = state[5];
                    state[5] = state[4];
                    state[4] = state[3] + temp1;
                    state[3] = state[2];
                    state[2] = state[1];
                    state[1] = state[0];
                    state[0] = temp1 + temp2;
                }

                for (int index = 0; index < 8; ++index)
                {
                    m_State[index] += state[index];
                }
            }

        public:

            std::vector< uint8_t > Finish()
            {
                Pad( true, 8 );
                std::vector< uint8_t > out( 32 );

                for (int index = 0; index < 32; ++index)
                {
                    out[index] = static_cast<uint8_t>( m_State[index / 4] >> (24 - (index % 4) * 8) );
                }

                return out;
            }
        };

        // SHA-512 and SHA-384 [same transform, other initial state, truncated output].
        class Sha512Hasher : public BlockHasher< 128 >
        {
        private:

            uint64_t m_State[8];
            size_t m_OutputLength;

            static uint64_t Rotr64(
                uint64_t value,
                int count
            )
            {
                return (value >> count) | (value << (64 - count));
            }

            void Transform(
                const uint8_t * block
            )
            {
                static const uint64_t TABLE[80] =
                {
                    0x428A2F98D728AE22, 0x7137449123EF65CD, 0xB5C0FBCFEC4D3B2F, 0xE9B5DBA58189DBBC, 0x3956C25BF348B538, 0x59F111F1B605D019,
                    0x923F82A4AF194F9B, 0xAB1C5ED5DA6D8118, 0xD807AA98A3030242, 0x12835B0145706FBE, 0x243185BE4EE4B28C, 0x550C7DC3D5FFB4E2,
                    0x72BE5D74F27B896F, 0x80DEB1FE3B1696B1, 0x9BDC06A725C71235, 0xC19BF174CF692694, 0xE49B69C19EF14AD2, 0xEFBE4786384F25E3,
                    0x0FC19DC68B8CD5B5, 0x240CA1CC77AC9C65, 0x2DE92C6F592B0275, 0x4A7484AA6EA6E483, 0x5CB0A9DCBD41FBD4, 0x76F988DA831153B5,
                    0x983E5152EE66DFAB, 0xA831C66D2DB43210, 0xB00327C898FB213F, 0xBF597FC7BEEF0EE4, 0xC6E00BF33DA88FC2, 0xD5A79147930AA725,
                    0x06CA6351E003826F, 0x142929670A0E6E70, 0x27B70A8546D22FFC, 0x2E1B21385C26C926, 0x4D2C6DFC5AC42AED, 0x53380D139D95B3DF,
                    0x650A73548BAF63DE, 0x766A0ABB3C77B2A8, 0x81C2C92E47EDAEE6, 0x92722C851482353B, 0xA2BFE8A14CF10364, 0xA81A664BBC423001,
                    0xC24B8B70D0F89791, 0xC76C51A30654BE30, 0xD192E819D6EF5218, 0xD69906245565A910, 0xF40E35855771202A, 0x106AA07032BBD1B8,
                    0x19A4C116B8D2D0C8, 0x1E376C085141AB53, 0x2748774CDF8EEB99, 0x34B0BCB5E19B48A8, 0x391C0CB3C5C95A63, 0x4ED8AA4AE3418ACB,
                    0x5B9CCA4F7763E373, 0x682E6FF3D6B2B8A3, 0x748F82EE5DEFB2FC, 0x78A5636F43172F60, 0x84C87814A1F0AB72, 0x8CC702081A6439EC,
                    0x90BEFFFA23631E28, 0xA4506CEBDE82BDE9, 0xBEF9A3F7B2C67915, 0xC67178F2E372532B, 0xCA273ECEEA26619C, 0xD186B8C721C0C207,
                    0xEADA7DD6CDE0EB1E, 0xF57D4F7FEE6ED178, 0x06F067AA72176FBA, 0x0A637DC5A2C898A6, 0x113F9804BEF90DAE, 0x1B710B35131C471B,
                    0x28DB77F523047D84, 0x32CAAB7B40C72493, 0x3C9EBE0A15C9BEBC, 0x431D67C49C100D4C, 0x4CC5D4BECB3E42B6, 0x597F299CFC657E2A,
                    0x5FCB6FAB3AD6FAEC, 0x6C44198C4A475817
                };

                uint64_t words[80];

                for (int index = 0; index < 16; ++index)
                {
                    words[index] = 0;

                    for (int byte = 0; byte < 8; ++byte)
                    {
                        words[index] = words[index] << 8 | block[index * 8 + byte];
                    }
                }

                for (int index = 16; index < 80; ++index)
                {
                    uint64_t s0 = Rotr64( words[index - 15], 1 ) ^ Rotr64( words[index - 15], 8 ) ^ (words[index - 15] >> 7);
                    uint64_t s1 = Rotr64( words[index - 2], 19 ) ^ Rotr64( words[index - 2], 61 ) ^ (words[index - 2] >> 6);
                    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
                }

                uint64_t state[8];
                std::memcpy( state, m_State, sizeof( state ) );

                for (int round = 0; round < 80; ++round)
                {
                    uint64_t s1 = Rotr64( state[4], 14 ) ^ Rotr64( state[4], 18 ) ^ Rotr64( state[4], 41 );
                    uint64_t choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
                    uint64_t temp1 = state[7] + s1 + choice + TABLE[round] + words[round];
                    uint64_t s0 = Rotr64( state[0], 28 ) ^ Rotr64( state[0], 34 ) ^ Rotr64( state[0], 39 );
                    uint64_t majority = (state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]);
                    uint64_t temp2 = s0 + majority;

                    state[7] = state[6];
                    state[6] = state[5];
                    state[5] = state[4];
                    state[4] = state[3] + temp1;
                    state[3] = state[2];
                    state[2] = state[1];
                    state[1] = state[0];
                    state[0] = temp1 + temp2;
                }

                for (int index = 0; index < 8; ++index)
                {
                    m_State[index] += state[index];
                }
            }

        public:

            explicit Sha512Hasher(
                bool sha384
            )
                : m_OutputLength( sha384 ? 48 : 64 )
            {
                static const uint64_t SHA512_INIT[8] = { 0x6A09E667F3BCC908, 0xBB67AE8584CAA73B, 0x3C6EF372FE94F82B, 0xA54FF53A5F1D36F1,
                    0x510E527FADE682D1, 0x9B05688C2B3E6C1F, 0x1F83D9ABFB41BD6B, 0x5BE0CD19137E2179 };
                static const uint64_t SHA384_INIT[8] = { 0xCBBB9D5DC1059ED8, 0x629A292A367CD507, 0x9159015A3070DD17, 0x152FECD8F70E5939,
                    0x67332667FFC00B31, 0x8EB44A8768581511, 0xDB0C2E0D64F98FA7, 0x47B5481DBEFA4FA4 };
                std::memcpy( m_State, sha384 ? SHA384_INIT : SHA512_INIT, sizeof( m_State ) );
            }

            std::vector< uint8_t > Finish()
            {
                Pad( true, 16 );
                std::vector< uint8_t > out( m_OutputLength );

                for (size_t index = 0; index < m_OutputLength; ++index)
                {
                    out[index] = static_cast<uint8_t>( m_State[index / 8] >> (56 - (index % 8) * 8) );
                }

                return out;
            }
        };
    }

    #ifdef _WIN32
    namespace
    {
        // Algorithm providers are opened once and kept for the process.
        BCRYPT_ALG_HANDLE SystemProvider(
            HashAlgorithm algorithm
        )
        {
            auto open = [] ( LPCWSTR name )
            {
                BCRYPT_ALG_HANDLE handle = nullptr;
                return BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &handle, name, nullptr, 0 ) ) ? handle : nullptr;
            };

            static BCRYPT_ALG_HANDLE md5 = open( BCRYPT_MD5_ALGORITHM );
            static BCRYPT_ALG_HANDLE sha1 = open( BCRYPT_SHA1_ALGORITHM );
            static BCRYPT_ALG_HANDLE sha256 = open( BCRYPT_SHA256_ALGORITHM );
            static BCRYPT_ALG_HANDLE sha384 = open( BCRYPT_SHA384_ALGORITHM );
            static BCRYPT_ALG_HANDLE sha512 = open( BCRYPT_SHA512_ALGORITHM );

            switch (algorithm)
            {
                case HashAlgorithm::HASH_MD5:
                    return md5;
                case HashAlgorithm::HASH_SHA1:
                    return sha1;
                case HashAlgorithm::HASH_SHA256:
                    return sha256;
                case HashAlgorithm::HASH_SHA384:
                    return sha384;
                case HashAlgorithm::HASH_SHA512:
                    return sha512;
                default:
                    return nullptr;
            }
        }

        ULONG DigestLength(
            HashAlgorithm algorithm
        )
        {
            switch (algorithm)
            {
                case HashAlgorithm::HASH_MD5:
                    return 16;
                case HashAlgorithm::HASH_SHA1:
                    return 20;
                case HashAlgorithm::HASH_SHA256:
                    return 32;
                case HashAlgorithm::HASH_SHA384:
                    return 48;
                default:
                    return 64;
            }
        }
    }
    #endif

    struct Hasher::Implementation
    {
        HashAlgorithm m_Algorithm;
        std::unique_ptr< Md5Hasher > m_Md5;
        std::unique_ptr< Sha1Hasher > m_Sha1;
        std::unique_ptr< Sha256Hasher > m_Sha256;
        std::unique_ptr< Sha512Hasher > m_Sha512;
        std::unique_ptr< blake3_hasher > m_Blake3;
        #ifdef _WIN32
        BCRYPT_HASH_HANDLE m_System = nullptr;

        ~Implementation()
        {
            if (m_System != nullptr)
            {
                BCryptDestroyHash( m_System );
            }
        }
        #endif
    };

    Hasher::Hasher(
        HashAlgorithm algorithm
    )
        : m_Implementation( std::make_unique< Implementation >() )
    {
        m_Implementation->m_Algorithm = algorithm;

        #ifdef _WIN32
        if (BCRYPT_ALG_HANDLE provider = SystemProvider( algorithm ))
        {
            if (BCRYPT_SUCCESS( BCryptCreateHash( provider, &m_Implementation->m_System, nullptr, 0, nullptr, 0, 0 ) ))
            {
                return;
            }

            m_Implementation->m_System = nullptr;
        }
        #endif

        switch (algorithm)
        {
            case HashAlgorithm::HASH_MD5:
                m_Implementation->m_Md5 = std::make_unique< Md5Hasher >();
                break;
            case HashAlgorithm::HASH_SHA1:
                m_Implementation->m_Sha1 = std::make_unique< Sha1Hasher >();
                break;
            case HashAlgorithm::HASH_SHA256:
                m_Implementation->m_Sha256 = std::make_unique< Sha256Hasher >();
                break;
            case HashAlgorithm::HASH_SHA384:
            case HashAlgorithm::HASH_SHA512:
                m_Implementation->m_Sha512 = std::make_unique< Sha512Hasher >( algorithm == HashAlgorithm::HASH_SHA384 );
                break;
            case HashAlgorithm::HASH_BLAKE3:
                m_Implementation->m_Blake3 = std::make_unique< blake3_hasher >();
                blake3_hasher_init( m_Implementation->m_Blake3.get() );
                break;
        }
    }

    Hasher::~Hasher() = default;

    void Hasher::Update(
        std::span< const uint8_t > data
    )
    {
        Implementation & state = *m_Implementation;

        #ifdef _WIN32
        if (state.m_System != nullptr)
        {
            // BCryptHashData takes a ULONG length
            constexpr size_t CHUNK = 1u << 30;

            for (size_t done = 0; done < data.size(); done += CHUNK)
            {
                size_t length = std::min( CHUNK, data.size() - done );
                BCryptHashData( state.m_System, const_cast<PUCHAR>( data.data() + done ), static_cast<ULONG>( length ), 0 );
            }

            return;
        }
        #endif

        if (state.m_Md5)
        {
            state.m_Md5->Update( data.data(), data.size() );
        }
        else if (state.m_Sha1)
        {
            state.m_Sha1->Update( data.data(), data.size() );
        }
        else if (state.m_Sha256)
        {
            state.m_Sha256->Update( data.data(), data.size() );
        }
        else if (state.m_Sha512)
        {
            state.m_Sha512->Update( data.data(), data.size() );
        }
        else if (state.m_Blake3)
        {
            blake3_hasher_update( state.m_Blake3.get(), data.data(), data.size() );
        }
    }

    std::vector< uint8_t > Hasher::Finish()
    {
        Implementation & state = *m_Implementation;

        #ifdef _WIN32
        if (state.m_System != nullptr)
        {
            std::vector< uint8_t > digest( DigestLength( state.m_Algorithm ) );
            BCryptFinishHash( state.m_System, digest.data(), static_cast<ULONG>(digest.size()), 0 );
            BCryptDestroyHash( state.m_System );
            state.m_System = nullptr;
            return digest;
        }
        #endif

        if (state.m_Md5)
        {
            return state.m_Md5->Finish();
        }

        if (state.m_Sha1)
        {
            return state.m_Sha1->Finish();
        }

        if (state.m_Sha256)
        {
            return state.m_Sha256->Finish();
        }

        if (state.m_Sha512)
        {
            return state.m_Sha512->Finish();
        }

        std::vector< uint8_t > digest( BLAKE3_OUT_LEN );
        blake3_hasher_finalize( state.m_Blake3.get(), digest.data(), digest.size() );
        return digest;
    }

    std::vector< uint8_t > ComputeHash(
        HashAlgorithm algorithm,
        std::span< const uint8_t > data
    )
    {
        Hasher hasher( algorithm );
        hasher.Update( data );
        return hasher.Finish();
    }

    std::string HashToHex(
        const std::vector< uint8_t > & digest,
        bool upper_case
    )
    {
        static const char * UPPER = "0123456789ABCDEF";
        static const char * LOWER = "0123456789abcdef";
        const char * digits = upper_case ? UPPER : LOWER;
        std::string text;
        text.reserve( digest.size() * 2 );

        for (uint8_t byte : digest)
        {
            text.push_back( digits[byte >> 4] );
            text.push_back( digits[byte & 0x0F] );
        }

        return text;
    }

    std::string ComputeHashHex(
        HashAlgorithm algorithm,
        std::span< const uint8_t > data
    )
    {
        return HashToHex( ComputeHash( algorithm, data ), true );
    }

    FileHashes ComputeFileHashes(
        std::span< const uint8_t > data
    )
    {
        // Small files are not worth the thread start.
        constexpr size_t PARALLEL_THRESHOLD = 1 << 20;

        if (data.size() < PARALLEL_THRESHOLD)
        {
            return FileHashes { ComputeHashHex( HashAlgorithm::HASH_MD5, data ), ComputeHashHex( HashAlgorithm::HASH_SHA1, data ),
                ComputeHashHex( HashAlgorithm::HASH_SHA256, data ), ComputeHashHex( HashAlgorithm::HASH_BLAKE3, data ) };
        }

        auto md5 = std::async( std::launch::async, [data]
        {
            return ComputeHashHex( HashAlgorithm::HASH_MD5, data );
        } );
        auto sha1 = std::async( std::launch::async, [data]
        {
            return ComputeHashHex( HashAlgorithm::HASH_SHA1, data );
        } );
        auto sha256 = std::async( std::launch::async, [data]
        {
            return ComputeHashHex( HashAlgorithm::HASH_SHA256, data );
        } );
        std::string blake3 = ComputeHashHex( HashAlgorithm::HASH_BLAKE3, data );
        return FileHashes { md5.get(), sha1.get(), sha256.get(), std::move( blake3 ) };
    }
}