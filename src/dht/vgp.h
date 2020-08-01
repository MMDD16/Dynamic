// Copyright (c) 2020-present Duality Blockchain Solutions Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <string>
#include <vector>

bool VgpEncrypt(const std::vector<std::vector<unsigned char>>& vvchPubKeys, 
                const std::vector<unsigned char>& vchValue, std::vector<unsigned char>& 
                vchEncrypted, std::string& strErrorMessage);

bool VgpDecrypt(const std::vector<unsigned char>& vchPrivSeedBytes, 
                const std::vector<unsigned char>& vchData, 
                std::vector<unsigned char>& vchDecrypted, std::string& strErrorMessage);