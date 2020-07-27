
// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bdap/certificate.h"
#include "bdap/utils.h"
#include "hash.h"
#include "script/script.h"
#include "streams.h"
#include "uint256.h"
#include "validation.h"

#include <libtorrent/ed25519.hpp>
#include <univalue.h>

void CCertificate::Serialize(std::vector<unsigned char>& vchData) 
{
    CDataStream dsEntryCertificate(SER_NETWORK, PROTOCOL_VERSION);
    dsEntryCertificate << *this;
    vchData = std::vector<unsigned char>(dsEntryCertificate.begin(), dsEntryCertificate.end());
}

bool CCertificate::UnserializeFromData(const std::vector<unsigned char>& vchData, const std::vector<unsigned char>& vchHash) 
{
    try {
        CDataStream dsEntryCertificate(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsEntryCertificate >> *this;

        std::vector<unsigned char> vchEntryLinkData;
        Serialize(vchEntryLinkData);
        const uint256 &calculatedHash = Hash(vchEntryLinkData.begin(), vchEntryLinkData.end());
        const std::vector<unsigned char> &vchRandEntryLink = vchFromValue(calculatedHash.GetHex());
        if(vchRandEntryLink != vchHash)
        {
            SetNull();
            return false;
        }
    } catch (std::exception &e) {
        SetNull();
        return false;
    }
    return true;
}

bool CCertificate::UnserializeFromTx(const CTransactionRef& tx, const unsigned int& height) 
{
    std::vector<unsigned char> vchData;
    std::vector<unsigned char> vchHash;
    int nOut;
    if(!GetBDAPData(tx, vchData, vchHash, nOut))
    {
        SetNull();
        return false;
    }
    if(!UnserializeFromData(vchData, vchHash))
    {
        return false;
    }

    //Distinguish between Request and Approve
    int op1, op2;
    std::vector<std::vector<unsigned char> > vvchBDAPArgs;
    CScript scriptOp;
    if (GetBDAPOpScript(tx, scriptOp, vvchBDAPArgs, op1, op2)) {
        std::string errorMessage;
        std::string strOpType = GetBDAPOpTypeString(op1, op2);
        if (strOpType == "bdap_new_certificate") {
            txHashRequest = tx->GetHash();
            nHeightRequest = height;
        }
        else if (strOpType == "bdap_approve_certificate") {
            txHashApprove = tx->GetHash();
            nHeightApprove = height;
        }
        //TODO: bdap_revoke_certificate?
    }

    return true;
}

std::string CCertificate::GetPubKeyHex() const
{
    std::vector<unsigned char> certPubKey = PublicKey;
    
    return ToHex(&certPubKey[0], certPubKey.size());
}

std::string CCertificate::GetSubjectSignature() const
{
    std::vector<unsigned char> subjectSig = SubjectSignature;

    return EncodeBase64(&subjectSig[0], subjectSig.size());
}

std::string CCertificate::GetSignatureValue() const
{
    std::vector<unsigned char> issuerSig = SignatureValue;

    return EncodeBase64(&issuerSig[0], issuerSig.size());
}

uint256 CCertificate::GetHash() const
{
    CDataStream dsCertificate(SER_NETWORK, PROTOCOL_VERSION);
    dsCertificate << *this;
    return Hash(dsCertificate.begin(), dsCertificate.end());
}

uint256 CCertificate::GetSubjectHash() const
{
    CDataStream dsCertificate(SER_NETWORK, PROTOCOL_VERSION);
    //dsCertificate << Subject << SignatureAlgorithm << SignatureHashAlgorithm << SerialNumber;
    dsCertificate << SignatureAlgorithm << SignatureHashAlgorithm << Subject << SerialNumber << KeyUsage << ExtendedKeyUsage << AuthorityInformationAccess << SubjectAlternativeName << Policies << CRLDistributionPoints << SCTList;
    return Hash(dsCertificate.begin(), dsCertificate.end());
}

uint256 CCertificate::GetIssuerHash() const
{
    CDataStream dsCertificate(SER_NETWORK, PROTOCOL_VERSION);
    //dsCertificate << Issuer << Subject << SignatureAlgorithm << SignatureHashAlgorithm << SerialNumber;
    dsCertificate << SignatureAlgorithm << SignatureHashAlgorithm << MonthsValid << Subject << SubjectSignature << Issuer << PublicKey << SerialNumber << KeyUsage << ExtendedKeyUsage << AuthorityInformationAccess << SubjectAlternativeName << Policies << CRLDistributionPoints << SCTList;
    return Hash(dsCertificate.begin(), dsCertificate.end());
}

bool CCertificate::SignSubject(const std::vector<unsigned char>& vchPubKey, const std::vector<unsigned char>& vchPrivKey)
{
    std::vector<unsigned char> msg = vchFromString(GetSubjectHash().ToString());
    std::vector<unsigned char> sig(64);

    libtorrent::ed25519_sign(&sig[0], &msg[0], msg.size(), &vchPubKey[0], &vchPrivKey[0]);
    SubjectSignature = sig;

    return true;
}

bool CCertificate::SignIssuer(const std::vector<unsigned char>& vchPubKey, const std::vector<unsigned char>& vchPrivKey)
{
    std::vector<unsigned char> msg = vchFromString(GetIssuerHash().ToString());
    std::vector<unsigned char> sig(64);

    libtorrent::ed25519_sign(&sig[0], &msg[0], msg.size(), &vchPubKey[0], &vchPrivKey[0]);
    SignatureValue = sig;

    return true;
}

bool CCertificate::CheckSubjectSignature(const std::vector<unsigned char>& vchPubKey) const
{
    std::vector<unsigned char> msg = vchFromString(GetSubjectHash().ToString());

    if (!libtorrent::ed25519_verify(&SubjectSignature[0], &msg[0], msg.size(), &vchPubKey[0])) {
        return false;
    }

    return true;
}

bool CCertificate::CheckIssuerSignature(const std::vector<unsigned char>& vchPubKey) const
{
    std::vector<unsigned char> msg = vchFromString(GetIssuerHash().ToString());

    if (!libtorrent::ed25519_verify(&SignatureValue[0], &msg[0], msg.size(), &vchPubKey[0])) {
        return false;
    }

    return true;
}

bool CCertificate::ValidateValues(std::string& errorMessage) const
{
    //Check that Subject exists
    if (Subject.size() == 0)
    {
        errorMessage = "Subject cannot be empty.";
        return false;
    }

    //Check that Subject signature exists
    if (SubjectSignature.size() == 0)
    {
        errorMessage = "Subject Signature cannot be empty.";
        return false;
    }

    //Check that PublicKey exists
    if (PublicKey.size() == 0)
    {
        errorMessage = "Public Key cannot be empty.";
        return false;
    }

    // check Signature Algorithm
    if (SignatureAlgorithm.size() > MAX_ALGORITHM_TYPE_LENGTH) 
    {
        errorMessage = "Invalid Signature Algorithm. Can not have more than " + std::to_string(MAX_ALGORITHM_TYPE_LENGTH) + " characters.";
        return false;
    }

    // check SignatureHashAlgorithm
    if (SignatureHashAlgorithm.size() > MAX_ALGORITHM_TYPE_LENGTH) 
    {
        errorMessage = "Invalid Signature Hash Algorithm. Can not have more than " + std::to_string(MAX_ALGORITHM_TYPE_LENGTH) + " characters.";
        return false;
    }

    // check FingerPrint
    if (FingerPrint.size() > MAX_CERTIFICATE_FINGERPRINT) 
    {
        errorMessage = "Invalid Finger Print. Can not have more than " + std::to_string(MAX_CERTIFICATE_FINGERPRINT) + " characters.";
        return false;
    }

    // check subject owner path
    if (Subject.size() > MAX_OBJECT_FULL_PATH_LENGTH) 
    {
        errorMessage = "Invalid Subject full path name. Can not have more than " + std::to_string(MAX_OBJECT_FULL_PATH_LENGTH) + " characters.";
        return false;
    }

    // check SubjectSignature
    if (SubjectSignature.size() > MAX_CERTIFICATE_SIGNATURE_LENGTH) 
    {
        errorMessage = "Invalid SubjectSignature. Can not have more than " + std::to_string(MAX_CERTIFICATE_SIGNATURE_LENGTH) + " characters.";
        return false;
    }

    // check issuer owner path
    if (Issuer.size() > MAX_OBJECT_FULL_PATH_LENGTH) 
    {
        errorMessage = "Invalid Issuer full path name. Can not have more than " + std::to_string(MAX_OBJECT_FULL_PATH_LENGTH) + " characters.";
        return false;
    }

    // check PublicKey
    if (PublicKey.size() > MAX_CERTIFICATE_KEY_LENGTH) 
    {
        errorMessage = "Invalid PublicKey. Can not have more than " + std::to_string(MAX_CERTIFICATE_KEY_LENGTH) + " characters.";
        return false;
    }

    // check SignatureValue
    if (SignatureValue.size() > MAX_CERTIFICATE_SIGNATURE_LENGTH) 
    {
        errorMessage = "Invalid SignatureValue. Can not have more than " + std::to_string(MAX_CERTIFICATE_SIGNATURE_LENGTH) + " characters.";
        return false;
    }

    //check KeyUsage (amount of records, and length of each record)
    if (KeyUsage.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid KeyUsage size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& KeyUsageValue : KeyUsage) {
        if (KeyUsageValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid KeyUsage. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check ExtendedKeyUsage (amount of records, and length of each record)
    if (ExtendedKeyUsage.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid ExtendedKeyUsage size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& ExtendedKeyUsageValue : ExtendedKeyUsage) {
        if (ExtendedKeyUsageValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid ExtendedKeyUsage. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check AuthorityInformationAccess (amount of records, and length of each record)
    if (AuthorityInformationAccess.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid AuthorityInformationAccess size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& AuthorityInformationAccessValue : AuthorityInformationAccess) {
        if (AuthorityInformationAccessValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid AuthorityInformationAccess. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check SubjectAlternativeName (amount of records, and length of each record)
    if (SubjectAlternativeName.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid SubjectAlternativeName size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& SubjectAlternativeNameValue : SubjectAlternativeName) {
        if (SubjectAlternativeNameValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid SubjectAlternativeName. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check Policies (amount of records, and length of each record)
    if (Policies.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid Policies size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& PoliciesValue : Policies) {
        if (PoliciesValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid Policies. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check CRLDistributionPoints (amount of records, and length of each record)
    if (CRLDistributionPoints.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid CRLDistributionPoints size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& CRLDistributionPointsValue : CRLDistributionPoints) {
        if (CRLDistributionPointsValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid CRLDistributionPoints. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    //check SCTList (amount of records, and length of each record)
    if (SCTList.size() > MAX_CERTIFICATE_EXTENSION_RECORDS)
    {
        errorMessage = "Invalid SCTList size. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_RECORDS) + " records.";
        return false;
    }
    for (const CharString& SCTListValue : SCTList) {
        if (SCTListValue.size() > MAX_CERTIFICATE_EXTENSION_LENGTH) 
        {
            errorMessage = "Invalid SCTList. Can not have more than " + std::to_string(MAX_CERTIFICATE_EXTENSION_LENGTH) + " characters.";
            return false;
        }
    }

    return true;
}

std::string CCertificate::ToString() const
{
     return strprintf(
        "CCertificate(\n"
        "    nVersion                 = %d\n"
        "    Months Valid             = %d\n"
        "    Finger Print             = %d\n"
        "    Signature Algorithm      = %s\n"
        "    Signature Hash Algorithm = %s\n"
        "    Subject                  = %s\n"
        "    Subject Signature        = %s\n"
        "    PublicKey                = %s\n"
        "    Issuer                   = %s\n"
        "    Signature Value          = %s\n"
        "    Serial Number            = %d\n"
        "    Key ID                   = %d\n"
        "    Self Signed              = %s\n"
        "    Approved                 = %s\n"
        "    Request TxId             = %s\n"
        "    Approve TxId             = %s\n"
        ")\n",
        nVersion,
        MonthsValid,
        GetFingerPrint(),
        stringFromVch(SignatureAlgorithm),
        stringFromVch(SignatureHashAlgorithm),
        stringFromVch(Subject),
        GetSubjectSignature(),
        GetPubKeyHex(),
        stringFromVch(Issuer),
        GetSignatureValue(),
        SerialNumber,
        GetCertificateKeyID().ToString(),
        SelfSignedCertificate() ? "True" : "False",
        IsApproved() ? "True" : "False",
        txHashRequest.GetHex(),
        txHashApprove.GetHex()
        );
}

bool BuildCertificateJson(const CCertificate& certificate, UniValue& oCertificate)
{
    int64_t nTime = 0;
    int64_t nApproveTime = 0;

    UniValue oKeyUsages(UniValue::VOBJ);
    int counter = 0;
    for(const std::vector<unsigned char>& vchKeyUsage : certificate.KeyUsage) {
        counter++;
        oKeyUsages.push_back(Pair("key_usage" + std::to_string(counter), stringFromVch(vchKeyUsage)));
    }

    CKeyID certificateKeyId = certificate.GetCertificateKeyID();

    oCertificate.push_back(Pair("version", std::to_string(certificate.nVersion)));

    oCertificate.push_back(Pair("signature_algorithm", stringFromVch(certificate.SignatureAlgorithm)));
    oCertificate.push_back(Pair("signature_hash_algorithm", stringFromVch(certificate.SignatureHashAlgorithm)));
    oCertificate.push_back(Pair("fingerprint", certificate.GetFingerPrint()));
    oCertificate.push_back(Pair("months_valid", std::to_string(certificate.MonthsValid)));
    oCertificate.push_back(Pair("subject", stringFromVch(certificate.Subject)));
    oCertificate.push_back(Pair("subject_signature", certificate.GetSubjectSignature()));
    oCertificate.push_back(Pair("issuer", stringFromVch(certificate.Issuer)));
    oCertificate.push_back(Pair("public_key", certificate.GetPubKeyHex()));
    oCertificate.push_back(Pair("signature_value", certificate.GetSignatureValue()));
    oCertificate.push_back(Pair("approved", certificate.IsApproved() ? "True" : "False"));
    oCertificate.push_back(Pair("serial_number", std::to_string(certificate.SerialNumber)));

    oCertificate.push_back(Pair("certificate_keyid", certificateKeyId.ToString()));
    oCertificate.push_back(Pair("key_usage", oKeyUsages));

    oCertificate.push_back(Pair("txid_request", certificate.txHashRequest.GetHex()));
    oCertificate.push_back(Pair("txid_approve", certificate.txHashApprove.GetHex()));
    if ((unsigned int)chainActive.Height() >= certificate.nHeightRequest) {
        CBlockIndex *pindex = chainActive[certificate.nHeightRequest];
        if (pindex) {
            nTime = pindex->GetBlockTime();
        }
    }
    oCertificate.push_back(Pair("request_time", nTime));
    oCertificate.push_back(Pair("request_height", std::to_string(certificate.nHeightRequest)));

    if (certificate.nHeightApprove != 0) {
        if ((unsigned int)chainActive.Height() >= certificate.nHeightApprove) {
            CBlockIndex *pindex = chainActive[certificate.nHeightApprove];
            if (pindex) {
                nApproveTime = pindex->GetBlockTime();
            }
        }
        oCertificate.push_back(Pair("valid_from", nApproveTime));
        oCertificate.push_back(Pair("valid_until", AddMonthsToBlockTime(nApproveTime,certificate.MonthsValid)));
        oCertificate.push_back(Pair("approve_height", std::to_string(certificate.nHeightApprove)));
    }
    
    return true;
}
