#include "pecore/Authenticode.h"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace PeCore
{
    namespace
    {
        constexpr uint16_t WIN_CERT_TYPE_PKCS_SIGNED_DATA = 0x0002;

        // Object identifiers used below [dotted form].
        const char * OID_SIGNED_DATA = "1.2.840.113549.1.7.2";
        const char * OID_SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4";
        const char * OID_SPC_PE_IMAGE_DATA = "1.3.6.1.4.1.311.2.1.15";
        const char * OID_SPC_SP_OPUS_INFO = "1.3.6.1.4.1.311.2.1.12";
        const char * OID_NESTED_SIGNATURE = "1.3.6.1.4.1.311.2.4.1";
        const char * OID_RFC3161_TIMESTAMP = "1.3.6.1.4.1.311.3.3.1";
        const char * OID_COUNTER_SIGNATURE = "1.2.840.113549.1.9.6";
        const char * OID_SIGNING_TIME = "1.2.840.113549.1.9.5";
        const char * OID_MESSAGE_DIGEST = "1.2.840.113549.1.9.4";

        // DER element. Tag byte, the content and the whole encoding [tag, length and content].
        struct Der
        {
            uint8_t m_Tag = 0;
            std::span< const uint8_t > m_Content;
            std::span< const uint8_t > m_Encoding;

            bool IsValid() const
            {
                return m_Tag != 0 || !m_Encoding.empty();
            }
        };

        // Reads one element at the start of data. DER only [definite lengths, single byte tags], every length is checked
        // against the buffer so a damaged blob cannot read outside of it.
        bool ReadDer(
            std::span< const uint8_t > data,
            Der & element
        )
        {
            if (data.size() < 2 || (data[0] & 0x1F) == 0x1F)
            {
                return false;
            }

            size_t length = data[1];
            size_t header = 2;

            if (length & 0x80)
            {
                size_t bytes = length & 0x7F;

                if (bytes == 0 || bytes > 4 || data.size() < 2 + bytes)
                {
                    return false;
                }

                length = 0;

                for (size_t index = 0; index < bytes; ++index)
                {
                    length = length << 8 | data[2 + index];
                }

                header += bytes;
            }

            if (length > data.size() - header)
            {
                return false;
            }

            element.m_Tag = data[0];
            element.m_Content = data.subspan( header, length );
            element.m_Encoding = data.subspan( 0, header + length );
            return true;
        }

        // The children of a constructed element.
        std::vector< Der > Children(
            const Der & parent
        )
        {
            std::vector< Der > children;
            std::span< const uint8_t > rest = parent.m_Content;
            Der child;

            while (!rest.empty() && ReadDer( rest, child ))
            {
                children.push_back( child );
                rest = rest.subspan( child.m_Encoding.size() );
            }

            return children;
        }

        std::string Oid(
            const Der & element
        )
        {
            if (element.m_Tag != 0x06 || element.m_Content.empty())
            {
                return std::string();
            }

            std::string text = std::to_string( element.m_Content[0] / 40 ) + "." + std::to_string( element.m_Content[0] % 40 );
            uint64_t value = 0;

            for (size_t index = 1; index < element.m_Content.size(); ++index)
            {
                value = value << 7 | (element.m_Content[index] & 0x7F);

                if (!(element.m_Content[index] & 0x80))
                {
                    text += "." + std::to_string( value );
                    value = 0;
                }
            }

            return text;
        }

        std::string HexBytes(
            std::span< const uint8_t > bytes
        )
        {
            static const char * DIGITS = "0123456789ABCDEF";
            std::string text;

            for (uint8_t byte : bytes)
            {
                text.push_back( DIGITS[byte >> 4] );
                text.push_back( DIGITS[byte & 15] );
            }

            return text;
        }

        // Strings of names and 'SpcString'. Printable, IA5, UTF8, Teletex [treated as Latin-1] and BMP.
        std::string DerString(
            const Der & element
        )
        {
            std::span< const uint8_t > bytes = element.m_Content;

            if (element.m_Tag == 0x1E || element.m_Tag == 0x80)
            {
                std::u16string wide;

                for (size_t index = 0; index + 1 < bytes.size(); index += 2)
                {
                    wide.push_back( static_cast<char16_t>(bytes[index] << 8 | bytes[index + 1]) );
                }

                return Utf8FromUtf16( wide );
            }

            std::string text( bytes.begin(), bytes.end() );
            return element.m_Tag == 0x14 ? Utf8FromLatin1( text ) : text;
        }

        struct DigestAlgorithm
        {
            const char * m_Oid;
            const char * m_Name;
            HashAlgorithm m_Algorithm;
        };

        const DigestAlgorithm DIGESTS[] =
        {
            { "1.3.14.3.2.26", "SHA-1", HashAlgorithm::HASH_SHA1 },
            { "2.16.840.1.101.3.4.2.1", "SHA-256", HashAlgorithm::HASH_SHA256 },
            { "2.16.840.1.101.3.4.2.2", "SHA-384", HashAlgorithm::HASH_SHA384 },
            { "2.16.840.1.101.3.4.2.3", "SHA-512", HashAlgorithm::HASH_SHA512 },
            { "1.2.840.113549.2.5", "MD5", HashAlgorithm::HASH_MD5 }
        };

        const DigestAlgorithm * FindDigest(
            const std::string & oid
        )
        {
            for (const DigestAlgorithm & digest : DIGESTS)
            {
                if (oid == digest.m_Oid)
                {
                    return &digest;
                }
            }

            return nullptr;
        }

        std::string AlgorithmName(
            const std::string & oid
        )
        {
            static const std::map< std::string, std::string > NAMES =
            {
                { "1.2.840.113549.1.1.1", "RSA" },
                { "1.2.840.113549.1.1.5", "SHA-1 with RSA" },
                { "1.2.840.113549.1.1.11", "SHA-256 with RSA" },
                { "1.2.840.113549.1.1.12", "SHA-384 with RSA" },
                { "1.2.840.113549.1.1.13", "SHA-512 with RSA" },
                { "1.2.840.113549.1.1.10", "RSASSA-PSS" },
                { "1.2.840.10045.4.3.2", "ECDSA with SHA-256" },
                { "1.2.840.10045.4.3.3", "ECDSA with SHA-384" },
                { "1.2.840.10045.4.3.4", "ECDSA with SHA-512" },
                { "1.2.840.10045.2.1", "ECDSA" }
            };

            if (const DigestAlgorithm * digest = FindDigest( oid ))
            {
                return digest->m_Name;
            }

            auto found = NAMES.find( oid );
            return found != NAMES.end() ? found->second : oid;
        }

        // First child OID of an 'AlgorithmIdentifier'.
        std::string AlgorithmOid(
            const Der & identifier
        )
        {
            std::vector< Der > parts = Children( identifier );
            return parts.empty() ? std::string() : Oid( parts[0] );
        }

        void FormatName(
            const Der & name,
            std::string & full,
            std::string & common_name
        )
        {
            static const std::map< std::string, std::string > KEYS =
            {
                { "2.5.4.3", "CN" }, { "2.5.4.6", "C" }, { "2.5.4.7", "L" }, { "2.5.4.8", "ST" }, { "2.5.4.10", "O" }, { "2.5.4.11", "OU" },
                { "2.5.4.5", "SERIALNUMBER" }, { "2.5.4.9", "STREET" }, { "1.2.840.113549.1.9.1", "E" }, { "2.5.4.97", "ORGID" }
            };

            full.clear();
            common_name.clear();

            for (const Der & relative : Children( name ))
            {
                for (const Der & attribute : Children( relative ))
                {
                    std::vector< Der > parts = Children( attribute );

                    if (parts.size() < 2)
                    {
                        continue;
                    }

                    std::string oid = Oid( parts[0] );
                    std::string value = DerString( parts[1] );
                    auto key = KEYS.find( oid );
                    full += (full.empty() ? "" : ", ") + (key != KEYS.end() ? key->second : oid) + "=" + value;

                    if (oid == "2.5.4.3")
                    {
                        common_name = value;
                    }
                }
            }

            if (common_name.empty())
            {
                common_name = full;
            }
        }

        // UTCTime YYMMDDHHMMSSZ and GeneralizedTime YYYYMMDDHHMMSS[.fff]Z as 'YYYY-MM-DD HH:MM:SS UTC'.
        std::string FormatTime(
            const Der & element
        )
        {
            std::string text( element.m_Content.begin(), element.m_Content.end() );
            std::string year;

            if (element.m_Tag == 0x17 && text.size() >= 12)
            {
                int short_year = std::atoi( text.substr( 0, 2 ).c_str() );
                year = std::to_string( short_year < 50 ? 2000 + short_year : 1900 + short_year );
                text = text.substr( 2 );
            }
            else if (element.m_Tag == 0x18 && text.size() >= 14)
            {
                year = text.substr( 0, 4 );
                text = text.substr( 4 );
            }
            else
            {
                return text;
            }

            return year + "-" + text.substr( 0, 2 ) + "-" + text.substr( 2, 2 ) + " " + text.substr( 4, 2 ) + ":" + text.substr( 6, 2 ) + ":" +
                text.substr( 8, 2 ) + " UTC";
        }

        struct ParsedCertificate
        {
            CertificateInfo m_Info;
            std::span< const uint8_t > m_IssuerEncoding;
            std::span< const uint8_t > m_SerialContent;
        };

        bool ParseCertificate(
            const Der & certificate,
            ParsedCertificate & parsed
        )
        {
            std::vector< Der > parts = Children( certificate );

            if (certificate.m_Tag != 0x30 || parts.size() < 3)
            {
                return false;
            }

            std::vector< Der > tbs = Children( parts[0] );
            size_t index = !tbs.empty() && tbs[0].m_Tag == 0xA0 ? 1 : 0;

            if (tbs.size() < index + 6)
            {
                return false;
            }

            const Der & serial = tbs[index];
            const Der & issuer = tbs[index + 2];
            std::vector< Der > validity = Children( tbs[index + 3] );
            const Der & subject = tbs[index + 4];

            CertificateInfo & info = parsed.m_Info;
            std::span< const uint8_t > serial_bytes = serial.m_Content;

            while (serial_bytes.size() > 1 && serial_bytes[0] == 0)
            {
                serial_bytes = serial_bytes.subspan( 1 );
            }

            info.m_Serial = HexBytes( serial_bytes );
            FormatName( issuer, info.m_Issuer, info.m_IssuerName );
            FormatName( subject, info.m_Subject, info.m_SubjectName );

            if (validity.size() >= 2)
            {
                info.m_ValidFrom = FormatTime( validity[0] );
                info.m_ValidTo = FormatTime( validity[1] );
            }

            info.m_SignatureAlgorithm = AlgorithmName( AlgorithmOid( parts[1] ) );
            info.m_Thumbprint = ComputeHashHex( HashAlgorithm::HASH_SHA1, certificate.m_Encoding );
            parsed.m_IssuerEncoding = issuer.m_Encoding;
            parsed.m_SerialContent = serial.m_Content;
            return true;
        }

        // Signed data parts used by Authenticode signatures and by RFC 3161 timestamp tokens
        struct SignedData
        {
            std::string m_ContentType;
            Der m_Content;
            std::vector< ParsedCertificate > m_Certificates;
            std::vector< Der > m_SignerInfos;
        };

        // ContentInfo ::= SEQUENCE { contentType, [0] EXPLICIT SignedData }
        bool ParseSignedData(
            const Der & content_info,
            SignedData & signed_data
        )
        {
            std::vector< Der > outer = Children( content_info );

            if (outer.size() < 2 || Oid( outer[0] ) != OID_SIGNED_DATA || outer[1].m_Tag != 0xA0)
            {
                return false;
            }

            std::vector< Der > wrapper = Children( outer[1] );

            if (wrapper.empty())
            {
                return false;
            }

            std::vector< Der > parts = Children( wrapper[0] );

            if (parts.size() < 4)
            {
                return false;
            }

            // version, digestAlgorithms, encapContentInfo, [0] certificates, [1] crls, signerInfos
            std::vector< Der > encapsulated = Children( parts[2] );

            if (!encapsulated.empty())
            {
                signed_data.m_ContentType = Oid( encapsulated[0] );
            }

            if (encapsulated.size() >= 2 && encapsulated[1].m_Tag == 0xA0)
            {
                std::vector< Der > content = Children( encapsulated[1] );

                if (!content.empty())
                {
                    signed_data.m_Content = content[0];
                }
            }

            for (size_t index = 3; index < parts.size(); ++index)
            {
                if (parts[index].m_Tag == 0xA0)
                {
                    for (const Der & certificate : Children( parts[index] ))
                    {
                        ParsedCertificate parsed;

                        if (ParseCertificate( certificate, parsed ))
                        {
                            signed_data.m_Certificates.push_back( parsed );
                        }
                    }
                }
                else if (parts[index].m_Tag == 0x31)
                {
                    signed_data.m_SignerInfos = Children( parts[index] );
                }
            }

            return true;
        }

        struct SignerFields
        {
            std::string m_DigestOid;
            std::string m_SignatureAlgorithm;
            int m_Certificate = -1;
            std::vector< Der > m_SignedAttributes;
            std::vector< Der > m_UnsignedAttributes;
        };

        // SignerInfo ::= SEQUENCE { version, sid, digestAlgorithm, [0] signedAttrs, signatureAlgorithm, signature,
        // [1] unsignedAttrs }, sid is IssuerAndSerialNumber here (subject key identifiers are not used by signtool)
        SignerFields ParseSigner(
            const Der & signer,
            const std::vector< ParsedCertificate > & certificates
        )
        {
            SignerFields fields;
            std::vector< Der > parts = Children( signer );

            if (parts.size() < 5)
            {
                return fields;
            }

            std::vector< Der > sid = Children( parts[1] );

            if (parts[1].m_Tag == 0x30 && sid.size() == 2)
            {
                for (size_t index = 0; index < certificates.size(); ++index)
                {
                    const ParsedCertificate & certificate = certificates[index];

                    if (std::ranges::equal( certificate.m_IssuerEncoding, sid[0].m_Encoding ) &&
                        std::ranges::equal( certificate.m_SerialContent, sid[1].m_Content ))
                    {
                        fields.m_Certificate = static_cast<int>( index );
                        break;
                    }
                }
            }

            fields.m_DigestOid = AlgorithmOid( parts[2] );
            size_t next = 3;

            if (parts[next].m_Tag == 0xA0)
            {
                fields.m_SignedAttributes = Children( parts[next] );
                ++next;
            }

            if (next < parts.size())
            {
                fields.m_SignatureAlgorithm = AlgorithmName( AlgorithmOid( parts[next] ) );
            }

            for (size_t index = next; index < parts.size(); ++index)
            {
                if (parts[index].m_Tag == 0xA1)
                {
                    fields.m_UnsignedAttributes = Children( parts[index] );
                }
            }

            return fields;
        }

        // Attribute ::= SEQUENCE { type, SET OF values }, returns the values of the attribute with the OID
        std::vector< Der > AttributeValues(
            const std::vector< Der > & attributes,
            const char * oid
        )
        {
            for (const Der & attribute : attributes)
            {
                std::vector< Der > parts = Children( attribute );

                if (parts.size() >= 2 && Oid( parts[0] ) == oid)
                {
                    return Children( parts[1] );
                }
            }

            return {};
        }

        std::string SignerName(
            const SignedData & signed_data,
            int certificate
        )
        {
            if (certificate >= 0)
            {
                return signed_data.m_Certificates[certificate].m_Info.m_SubjectName;
            }

            return signed_data.m_Certificates.empty() ? std::string() : signed_data.m_Certificates.front().m_Info.m_SubjectName;
        }

        void ReadTimestamp(
            const SignedData & outer,
            const SignerFields & signer,
            SignatureInfo & signature
        )
        {
            TimestampInfo & timestamp = signature.m_Timestamp;

            // RFC 3161: a complete SignedData with TSTInfo as content
            for (const Der & token : AttributeValues( signer.m_UnsignedAttributes, OID_RFC3161_TIMESTAMP ))
            {
                SignedData stamp;

                if (!ParseSignedData( token, stamp ))
                {
                    signature.m_Problems.push_back( "The RFC 3161 timestamp could not be read" );
                    continue;
                }

                timestamp.m_Present = true;
                timestamp.m_Kind = "RFC 3161";

                // eContent is an OCTET STRING holding TSTInfo ::= SEQUENCE { version, policy, messageImprint, serialNumber, genTime, ... }
                Der tst_info;

                if (stamp.m_Content.m_Tag == 0x04 && ReadDer( stamp.m_Content.m_Content, tst_info ))
                {
                    std::vector< Der > fields = Children( tst_info );

                    if (fields.size() >= 5)
                    {
                        timestamp.m_Time = FormatTime( fields[4] );
                        std::vector< Der > imprint = Children( fields[2] );

                        if (!imprint.empty())
                        {
                            timestamp.m_DigestAlgorithm = AlgorithmName( AlgorithmOid( imprint[0] ) );
                        }
                    }
                }

                int stamp_signer = stamp.m_SignerInfos.empty() ? -1 : ParseSigner( stamp.m_SignerInfos[0], stamp.m_Certificates ).m_Certificate;
                timestamp.m_Authority = SignerName( stamp, stamp_signer );

                for (size_t index = 0; index < stamp.m_Certificates.size(); ++index)
                {
                    CertificateInfo certificate = stamp.m_Certificates[index].m_Info;
                    certificate.m_Role = static_cast<int>( index ) == stamp_signer ? "Timestamp signer" : "Timestamp chain";
                    signature.m_Certificates.push_back( certificate );
                }

                return;
            }

            // Legacy Authenticode timestamp: a SignerInfo whose certificate is in the outer certificate list
            for (const Der & counter : AttributeValues( signer.m_UnsignedAttributes, OID_COUNTER_SIGNATURE ))
            {
                SignerFields fields = ParseSigner( counter, outer.m_Certificates );
                timestamp.m_Present = true;
                timestamp.m_Kind = "Authenticode (counter signature)";
                timestamp.m_DigestAlgorithm = AlgorithmName( fields.m_DigestOid );
                std::vector< Der > times = AttributeValues( fields.m_SignedAttributes, OID_SIGNING_TIME );
                timestamp.m_Time = times.empty() ? std::string() : FormatTime( times[0] );
                timestamp.m_Authority = fields.m_Certificate >= 0 ? outer.m_Certificates[fields.m_Certificate].m_Info.m_SubjectName : std::string();

                if (fields.m_Certificate >= 0)
                {
                    signature.m_Certificates[fields.m_Certificate].m_Role = "Timestamp signer";
                }

                return;
            }
        }

        void ParseSignature(
            const Der & content_info,
            int depth,
            const PeImage & image,
            std::map< HashAlgorithm, std::vector< uint8_t > > & digests,
            std::vector< SignatureInfo > & signatures
        )
        {
            SignatureInfo signature;
            signature.m_Depth = depth;
            SignedData signed_data;

            if (!ParseSignedData( content_info, signed_data ))
            {
                signature.m_Problems.push_back( "The certificate is not PKCS #7 SignedData" );
                signatures.push_back( signature );
                return;
            }

            for (const ParsedCertificate & certificate : signed_data.m_Certificates)
            {
                signature.m_Certificates.push_back( certificate.m_Info );
            }

            // SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue, messageDigest DigestInfo }
            std::vector< Der > indirect = Children( signed_data.m_Content );

            if (signed_data.m_ContentType != OID_SPC_INDIRECT_DATA || indirect.size() < 2)
            {
                signature.m_Problems.push_back( "The signed content is not SpcIndirectDataContent" );
            }
            else
            {
                std::vector< Der > data = Children( indirect[0] );

                if (!data.empty() && Oid( data[0] ) == OID_SPC_PE_IMAGE_DATA && data.size() >= 2)
                {
                    // Page hashes live in a serialized attribute deep inside SpcPeImageData, the encoded OID
                    // 1.3.6.1.4.1.311.2.3.1 (SHA-1) or .2 (SHA-256) is enough to tell
                    std::string encoded( data[1].m_Encoding.begin(), data[1].m_Encoding.end() );
                    signature.m_PageHashes = encoded.find( "\x2B\x06\x01\x04\x01\x82\x37\x02\x03\x01" ) != std::string::npos ||
                        encoded.find( "\x2B\x06\x01\x04\x01\x82\x37\x02\x03\x02" ) != std::string::npos;
                }

                std::vector< Der > digest_info = Children( indirect[1] );

                if (digest_info.size() >= 2)
                {
                    std::string oid = AlgorithmOid( digest_info[0] );
                    const DigestAlgorithm * algorithm = FindDigest( oid );
                    signature.m_DigestAlgorithm = algorithm != nullptr ? algorithm->m_Name : oid;
                    signature.m_SignedDigest.assign( digest_info[1].m_Content.begin(), digest_info[1].m_Content.end() );

                    if (algorithm != nullptr)
                    {
                        signature.m_AlgorithmSupported = true;
                        auto cached = digests.find( algorithm->m_Algorithm );

                        if (cached == digests.end())
                        {
                            cached = digests.emplace( algorithm->m_Algorithm, ComputeAuthenticodeDigest( image, algorithm->m_Algorithm ) ).first;
                        }

                        signature.m_FileDigest = cached->second;
                        signature.m_DigestMatches = signature.m_FileDigest == signature.m_SignedDigest;
                    }
                    else
                    {
                        signature.m_Problems.push_back( "Unknown digest algorithm " + oid );
                    }
                }
            }

            if (signed_data.m_SignerInfos.empty())
            {
                signature.m_Problems.push_back( "The signature has no signer" );
                signatures.push_back( signature );
                return;
            }

            SignerFields signer = ParseSigner( signed_data.m_SignerInfos[0], signed_data.m_Certificates );
            signature.m_SignerIndex = signer.m_Certificate;
            signature.m_SignatureAlgorithm = signer.m_SignatureAlgorithm;

            // SignerInfo names the key algorithm only ("RSA"), the digest comes from digestAlgorithm
            if (const DigestAlgorithm * digest = FindDigest( signer.m_DigestOid ); digest != nullptr && signature.m_SignatureAlgorithm.find( "with" ) == std::string::npos)
            {
                signature.m_SignatureAlgorithm += std::string( " with " ) + digest->m_Name;
            }

            if (signer.m_Certificate >= 0)
            {
                signature.m_Certificates[signer.m_Certificate].m_Role = "Signer";
            }
            else
            {
                signature.m_Problems.push_back( "The signer certificate is not part of the signature" );
            }

            // The messageDigest attribute covers the content octets of SpcIndirectDataContent (without its header)
            std::vector< Der > message_digest = AttributeValues( signer.m_SignedAttributes, OID_MESSAGE_DIGEST );
            const DigestAlgorithm * signer_digest = FindDigest( signer.m_DigestOid );

            if (!message_digest.empty() && signer_digest != nullptr && signed_data.m_Content.IsValid())
            {
                signature.m_ContentDigestChecked = true;
                std::vector< uint8_t > expected( message_digest[0].m_Content.begin(), message_digest[0].m_Content.end() );
                signature.m_ContentDigestMatches = ComputeHash( signer_digest->m_Algorithm, signed_data.m_Content.m_Content ) == expected;
            }

            std::vector< Der > times = AttributeValues( signer.m_SignedAttributes, OID_SIGNING_TIME );

            if (!times.empty())
            {
                signature.m_SigningTime = FormatTime( times[0] );
            }

            // SpcSpOpusInfo ::= SEQUENCE { [0] EXPLICIT SpcString programName, [1] EXPLICIT SpcLink moreInfo }
            std::vector< Der > opus = AttributeValues( signer.m_SignedAttributes, OID_SPC_SP_OPUS_INFO );

            if (!opus.empty())
            {
                for (const Der & field : Children( opus[0] ))
                {
                    std::vector< Der > inner = Children( field );

                    if (inner.empty())
                    {
                        continue;
                    }

                    if (field.m_Tag == 0xA0)
                    {
                        signature.m_ProgramName = DerString( inner[0] );
                    }
                    else if (field.m_Tag == 0xA1 && inner[0].m_Tag == 0x80)
                    {
                        signature.m_MoreInfo = std::string( inner[0].m_Content.begin(), inner[0].m_Content.end() );
                    }
                }
            }

            ReadTimestamp( signed_data, signer, signature );
            signatures.push_back( signature );

            for (const Der & nested : AttributeValues( signer.m_UnsignedAttributes, OID_NESTED_SIGNATURE ))
            {
                ParseSignature( nested, depth + 1, image, digests, signatures );
            }
        }
    }

    bool AuthenticodeInfo::AllDigestsMatch() const
    {
        return m_Present && !m_Signatures.empty() &&
            std::ranges::all_of( m_Signatures, [] ( const SignatureInfo & signature )
        {
            return signature.m_DigestMatches;
        } );
    }

    std::string AuthenticodeInfo::SignerName() const
    {
        if (m_Signatures.empty() || m_Signatures[0].m_SignerIndex < 0)
        {
            return std::string();
        }

        return m_Signatures[0].m_Certificates[m_Signatures[0].m_SignerIndex].m_SubjectName;
    }

    std::vector< uint8_t > ComputeAuthenticodeDigest(
        const PeImage & image,
        HashAlgorithm algorithm
    )
    {
        std::span< const uint8_t > data = image.GetData();
        uint64_t checksum = image.GetOptionalHeaderOffset() + 64ull;
        uint64_t entry = image.GetDataDirectoryOffset() + DIR_SECURITY * 8ull;
        DataDirectory table = image.GetDirectory( DIR_SECURITY );
        uint64_t table_start = table.m_Size != 0 ? std::min< uint64_t >( table.m_Rva, data.size() ) : data.size();
        uint64_t table_end = table.m_Size != 0 ? std::min< uint64_t >( static_cast<uint64_t>(table.m_Rva) + table.m_Size, data.size() ) : data.size();
        Require( checksum + 4 <= entry && entry + 8 <= table_start, ErrorCode::ERR_PARSE, "The headers overlap the certificate table" );

        Hasher hasher( algorithm );
        hasher.Update( data.subspan( 0, checksum ) );
        hasher.Update( data.subspan( checksum + 4, entry - checksum - 4 ) );
        hasher.Update( data.subspan( entry + 8, table_start - entry - 8 ) );
        hasher.Update( data.subspan( table_end ) );
        return hasher.Finish();
    }

    std::vector< SignatureInfo > ParseAuthenticodeSignature(
        std::span< const uint8_t > blob,
        const PeImage & image
    )
    {
        std::vector< SignatureInfo > signatures;
        std::map< HashAlgorithm, std::vector< uint8_t > > digests;
        Der content_info;

        if (!ReadDer( blob, content_info ))
        {
            SignatureInfo broken;
            broken.m_Problems.push_back( "The certificate is not valid DER" );
            signatures.push_back( broken );
            return signatures;
        }

        ParseSignature( content_info, 0, image, digests, signatures );
        return signatures;
    }

    AuthenticodeInfo ReadAuthenticode(
        const PeImage & image
    )
    {
        AuthenticodeInfo info;

        if (!image.IsPe() || !image.HasDirectory( DIR_SECURITY ))
        {
            return info;
        }

        DataDirectory table = image.GetDirectory( DIR_SECURITY );
        info.m_TableOffset = table.m_Rva;
        info.m_TableSize = table.m_Size;
        info.m_Present = true;
        uint64_t end = std::min< uint64_t >( info.m_TableOffset + info.m_TableSize, image.GetFileSize() );

        if (info.m_TableOffset + info.m_TableSize > image.GetFileSize())
        {
            info.m_Problems.push_back( "The certificate table runs past the end of the file" );
        }

        for (uint64_t offset = info.m_TableOffset; offset + 8 <= end; )
        {
            uint32_t length = image.Read32( offset );
            uint16_t type = image.Read16( offset + 6 );

            if (length < 8 || offset + length > end)
            {
                info.m_Problems.push_back( "WIN_CERTIFICATE at " + Hex( offset ) + " has an invalid length" );
                break;
            }

            if (type == WIN_CERT_TYPE_PKCS_SIGNED_DATA)
            {
                std::vector< SignatureInfo > signatures = ParseAuthenticodeSignature( image.Slice( offset + 8, length - 8 ), image );
                info.m_Signatures.insert( info.m_Signatures.end(), signatures.begin(), signatures.end() );
            }

            offset += (length + 7ull) & ~7ull;
        }

        return info;
    }

    std::string DescribeAuthenticode(
        const AuthenticodeInfo & info
    )
    {
        if (!info.m_Present)
        {
            return "Not signed";
        }

        if (info.m_Signatures.empty())
        {
            return "Certificate table without an Authenticode signature";
        }

        std::string algorithms;

        for (const SignatureInfo & signature : info.m_Signatures)
        {
            algorithms += (algorithms.empty() ? "" : " + ") + signature.m_DigestAlgorithm;
        }

        std::string signer = info.SignerName();
        std::string text = (signer.empty() ? std::string( "Signed" ) : "Signed by " + signer) + ", " + algorithms;
        return text + (info.AllDigestsMatch() ? ", file digest matches" : ", FILE DIGEST MISMATCH (modified after signing)");
    }

    std::shared_ptr< const AuthenticodeInfo > GetAuthenticodeInfo(
        const Document & document,
        const PeImage & image
    )
    {
        return document.GetCachedObject< AuthenticodeInfo >( "authenticode", [&]
        {
            return ReadAuthenticode( image );
        } );
    }

    AuthenticodeSummary GetAuthenticodeSummary(
        const Document & document,
        const PeImage & image
    )
    {
        AuthenticodeSummary summary;
        summary.m_Signed = image.IsPe() && image.HasDirectory( DIR_SECURITY );

        if (!summary.m_Signed)
        {
            summary.m_Text = "Not signed";
            return summary;
        }

        std::shared_ptr< const AuthenticodeInfo > info = GetAuthenticodeInfo( document, image );
        summary.m_Matches = info->AllDigestsMatch();
        summary.m_Text = DescribeAuthenticode( *info );
        return summary;
    }
}