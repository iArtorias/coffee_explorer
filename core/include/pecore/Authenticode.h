#pragma once

#include "Document.h"
#include "Hash.h"
#include "PeImage.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/*
    Authenticode signatures in the certificate table [security directory]. Parsed without the Windows crypto API so the
    core stays portable and testable. PKCS #7 SignedData with 'SpcIndirectDataContent', the certificates, the signer, the
    signing time, the timestamp [legacy counter signature or RFC 3161] and nested signatures [dual SHA-1 or SHA-256].

    The file digest is recomputed over the image without the CheckSum field, the security directory entry and the
    certificate table, then compared with the signed digest. Checking the RSA/ECDSA signature itself and the trust
    chain is left to 'WinVerifyTrust' ['Platform.h', 'VerifyFileTrust'].
*/

namespace PeCore
{
    struct CertificateInfo
    {
        // RFC 4514 like 'CN=..., O=..., C=...' in the order of the certificate, and the common name alone.
        std::string m_Subject;
        std::string m_SubjectName;
        std::string m_Issuer;
        std::string m_IssuerName;
        std::string m_Serial;
        std::string m_ValidFrom;
        std::string m_ValidTo;

        // SHA-1 of the DER encoding, as Windows shows it.
        std::string m_Thumbprint;
        std::string m_SignatureAlgorithm;

        // Signer, timestamp signer or "" for chain certificates.
        std::string m_Role;
    };

    struct TimestampInfo
    {
        bool m_Present = false;

        // 'RFC 3161' or Authenticode [counter signature].
        std::string m_Kind;
        std::string m_Time;
        std::string m_Authority;
        std::string m_DigestAlgorithm;
    };

    struct SignatureInfo
    {
        // 0 for the signature in the certificate table, 1 for nested signatures inside it.
        int m_Depth = 0;
        std::string m_DigestAlgorithm;
        bool m_AlgorithmSupported = false;
        std::vector< uint8_t > m_SignedDigest;
        std::vector< uint8_t > m_FileDigest;
        bool m_DigestMatches = false;

        // 'messageDigest' of the signed attributes against the signed content, shows a damaged signature blob.
        bool m_ContentDigestChecked = false;
        bool m_ContentDigestMatches = false;

        // Page hashes in SpcPeImageData [signed with '/ph'].
        bool m_PageHashes = false;
        std::string m_SigningTime;
        std::string m_ProgramName;
        std::string m_MoreInfo;
        std::string m_SignatureAlgorithm;
        int m_SignerIndex = -1;
        std::vector< CertificateInfo > m_Certificates;
        TimestampInfo m_Timestamp;
        std::vector< std::string > m_Problems;
    };

    struct AuthenticodeInfo
    {
        bool m_Present = false;
        uint64_t m_TableOffset = 0;
        uint64_t m_TableSize = 0;
        std::vector< SignatureInfo > m_Signatures;
        std::vector< std::string > m_Problems;

        // Every signature parsed and its digest equals the digest of the file.
        bool AllDigestsMatch() const;

        // 'Microsoft Windows' of the first signature, empty when unsigned.
        std::string SignerName() const;
    };

    // Digest of the image as Authenticode defines it. Everything except the CheckSum field, the security directory
    // entry and the certificate table.
    std::vector< uint8_t > ComputeAuthenticodeDigest(
        const PeImage & image,
        HashAlgorithm algorithm
    );

    AuthenticodeInfo ReadAuthenticode(
        const PeImage & image
    );

    // Parses one PKCS #7 blob ['bCertificate' of a 'WIN_CERTIFICATE'), digests are compared against image.
    std::vector< SignatureInfo > ParseAuthenticodeSignature(
        std::span< const uint8_t > blob,
        const PeImage & image
    );

    // 'Signed by Microsoft Windows, SHA-256, file digest matches' for the overview.
    std::string DescribeAuthenticode(
        const AuthenticodeInfo & info
    );

    struct AuthenticodeSummary
    {
        bool m_Signed = false;
        bool m_Matches = false;
        std::string m_Text;
    };

    // R'eadAuthenticode' of the current bytes, computed once per document revision.
    std::shared_ptr< const AuthenticodeInfo > GetAuthenticodeInfo(
        const Document & document,
        const PeImage & image
    );

    // 'DescribeAuthenticode' and 'AllDigestsMatch' of the current bytes, computed once per document revision.
    AuthenticodeSummary GetAuthenticodeSummary(
        const Document & document,
        const PeImage & image
    );
}