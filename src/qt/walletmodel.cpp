// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodel.h"

#include "addresstablemodel.h"
#include "assettablemodel.h"
#include "consensus/validation.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "myrestrictedassettablemodel.h"
#include "paymentserver.h"
#include "recentrequeststablemodel.h"
#include "transactiontablemodel.h"

#include "base58.h"
#include "instantsend.h"
#include "keystore.h"
#include "net.h" // for g_connman
#include "privatesend-client.h"
#include "rpc/server.h"
#include "spork.h"
#include "sync.h"
#include "ui_interface.h"
#include "util.h" // for GetBoolArg
#include "validation.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h" // for BackupWallet

#include <stdint.h>

#include <boost/foreach.hpp>

#include <QDebug>
#include <QSet>
#include <QTimer>

WalletModel::WalletModel(const PlatformStyle* platformStyle, CWallet* _wallet, OptionsModel* _optionsModel, QObject* parent) : QObject(parent), wallet(_wallet), optionsModel(_optionsModel), addressTableModel(0),
                                                                                                                               transactionTableModel(0),
                                                                                                                               assetTableModel(0),
                                                                                                                               recentRequestsTableModel(0),
                                                                                                                               cachedBalance(0),
                                                                                                                               cachedTotal(0),
                                                                                                                               cachedStake(0),
                                                                                                                               cachedUnconfirmedBalance(0),
                                                                                                                               cachedImmatureBalance(0),
                                                                                                                               cachedAnonymizedBalance(0),
                                                                                                                               cachedWatchOnlyBalance(0),
                                                                                                                               cachedWatchUnconfBalance(0),
                                                                                                                               cachedWatchImmatureBalance(0),
                                                                                                                               cachedEncryptionStatus(Unencrypted),
                                                                                                                               cachedNumBlocks(0),
                                                                                                                               cachedTxLocks(0),
                                                                                                                               cachedPrivateSendRounds(0)
{
    fHaveWatchOnly = wallet->HaveWatchOnly();
    fForceCheckBalanceChanged = false;

    addressTableModel = new AddressTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(platformStyle, wallet, this);
    assetTableModel = new AssetTableModel(this);
    recentRequestsTableModel = new RecentRequestsTableModel(wallet, this);
    myRestrictedAssetsTableModel = new MyRestrictedAssetsTableModel(platformStyle, wallet, this);

    // This timer will be fired repeatedly to update the balance
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(pollBalanceChanged()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

CAmount WalletModel::getBalance(const CCoinControl* coinControl) const
{
    if (coinControl) {
        CAmount nBalance = 0;
        std::vector<COutput> vCoins;
        wallet->AvailableCoins(vCoins, true, coinControl);
        BOOST_FOREACH (const COutput& out, vCoins)
            if (out.fSpendable)
                nBalance += out.tx->tx->vout[out.i].nValue;

        return nBalance;
    }

    return wallet->GetBalance();
}

CAmount WalletModel::getTotal() const
{
    return wallet->GetTotal();
}

CAmount WalletModel::getStake() const
{
    return wallet->GetStake();
}

CAmount WalletModel::getAnonymizedBalance() const
{
    return wallet->GetAnonymizedBalance();
}

CAmount WalletModel::getUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedBalance();
}

CAmount WalletModel::getImmatureBalance() const
{
    return wallet->GetImmatureBalance();
}

bool WalletModel::haveWatchOnly() const
{
    return fHaveWatchOnly;
}

CAmount WalletModel::getWatchBalance() const
{
    return wallet->GetWatchOnlyBalance();
}

CAmount WalletModel::getWatchUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedWatchOnlyBalance();
}

CAmount WalletModel::getWatchImmatureBalance() const
{
    return wallet->GetImmatureWatchOnlyBalance();
}

CAmount WalletModel::getWatchStake() const
{
    return wallet->GetWatchOnlyStake();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if (cachedEncryptionStatus != newEncryptionStatus)
        Q_EMIT encryptionStatusChanged(newEncryptionStatus);
}


void WalletModel::pollBalanceChanged()
{
    // Get required locks upfront. This avoids the GUI from getting stuck on
    // periodical polls if the core is holding the locks for a longer time -
    // for example, during a wallet rescan.
    TRY_LOCK(cs_main, lockMain);
    if (!lockMain)
        return;
    TRY_LOCK(wallet->cs_wallet, lockWallet);
    if (!lockWallet)
        return;

    if (fForceCheckBalanceChanged || chainActive.Height() != cachedNumBlocks || privateSendClient.nPrivateSendRounds != cachedPrivateSendRounds || cachedTxLocks != nCompleteTXLocks) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        cachedNumBlocks = chainActive.Height();
        cachedPrivateSendRounds = privateSendClient.nPrivateSendRounds;

        checkBalanceChanged();
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();
        if(assetTableModel)
            assetTableModel->checkBalanceChanged();
    }
}

void WalletModel::checkBalanceChanged()
{
    CAmount newBalance = getBalance();
    CAmount newTotal = getTotal();
    CAmount newStake = getStake();
    CAmount newUnconfirmedBalance = getUnconfirmedBalance();
    CAmount newImmatureBalance = getImmatureBalance();
    CAmount newAnonymizedBalance = getAnonymizedBalance();
    CAmount newWatchOnlyBalance = 0;
    CAmount newWatchOnlyStake = 0;
    CAmount newWatchUnconfBalance = 0;
    CAmount newWatchImmatureBalance = 0;
    if (haveWatchOnly()) {
        newWatchOnlyBalance = getWatchBalance();
        newWatchOnlyStake = getWatchStake();
        newWatchUnconfBalance = getWatchUnconfirmedBalance();
        newWatchImmatureBalance = getWatchImmatureBalance();
    }

    if (cachedBalance != newBalance || cachedTotal != newTotal || cachedStake != newStake || cachedUnconfirmedBalance != newUnconfirmedBalance || cachedImmatureBalance != newImmatureBalance ||
        cachedAnonymizedBalance != newAnonymizedBalance || cachedTxLocks != nCompleteTXLocks ||
        cachedWatchOnlyBalance != newWatchOnlyBalance || cachedWatchOnlyStake != newWatchOnlyStake || cachedWatchUnconfBalance != newWatchUnconfBalance || cachedWatchImmatureBalance != newWatchImmatureBalance) {
        cachedBalance = newBalance;
        cachedTotal = newTotal;
        cachedStake = newStake;
        cachedUnconfirmedBalance = newUnconfirmedBalance;
        cachedImmatureBalance = newImmatureBalance;
        cachedAnonymizedBalance = newAnonymizedBalance;
        cachedTxLocks = nCompleteTXLocks;
        cachedWatchOnlyBalance = newWatchOnlyBalance;
        cachedWatchUnconfBalance = newWatchUnconfBalance;
        cachedWatchImmatureBalance = newWatchImmatureBalance;
        Q_EMIT balanceChanged(newBalance,  newTotal, newStake, newUnconfirmedBalance, newImmatureBalance, newAnonymizedBalance,
            newWatchOnlyBalance, newWatchOnlyStake, newWatchUnconfBalance, newWatchImmatureBalance);
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString& address, const QString& label, bool isMine, const QString& purpose, int status)
{
    if (addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString& address)
{
    CTxDestination dest = DecodeDestination(address.toStdString());
    return IsValidDestination(dest);
}

void WalletModel::updateAddressBookLabels(const CTxDestination& dest, const std::string& strName, const std::string& strPurpose)
{
    LOCK(wallet->cs_wallet);

    std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

    // Check if we have a new address or an updated label
    if (mi == wallet->mapAddressBook.end()) {
        wallet->SetAddressBook(dest, strName, strPurpose);
    } else if (mi->second.name != strName) {
        wallet->SetAddressBook(dest, strName, ""); // "" means don't change purpose
    }
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction& transaction, const CCoinControl& coinControl)
{
    if (fWalletUnlockMixStakeOnly)
        return MixStakeOnlyMode;

    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if (recipients.empty()) {
        return OK;
    }

    // This should never really happen, yet another safety check, just in case.
    if (wallet->IsLocked()) {
        return TransactionCreationFailed;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    Q_FOREACH (const SendCoinsRecipient& rcp, recipients) {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;

        if (rcp.paymentRequest.IsInitialized()) { // PaymentRequest...
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int i = 0; i < details.outputs_size(); i++) {
                const payments::Output& out = details.outputs(i);
                if (out.amount() <= 0)
                    continue;
                subtotal += out.amount();
                const unsigned char* scriptStr = (const unsigned char*)out.script().data();
                CScript scriptPubKey(scriptStr, scriptStr + out.script().size());
                CAmount nAmount = out.amount();
                CRecipient recipient = {scriptPubKey, nAmount, rcp.fSubtractFeeFromAmount};
                vecSend.push_back(recipient);
            }
            if (subtotal <= 0) {
                return InvalidAmount;
            }
            total += subtotal;
        } else { // User-entered dynamic address / amount:
            if (!validateAddress(rcp.address)) {
                return InvalidAddress;
            }
            if (rcp.amount <= 0) {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CScript scriptPubKey;
            std::vector<uint8_t> vStealthData;
            bool fStealthAddress = false;
            CTxDestination dest = DecodeDestination(rcp.address.toStdString());
            if (!IsValidDestination(dest))
                return InvalidAddress;

            if (dest.type() == typeid(CStealthAddress))
            {
                CStealthAddress sxAddr = boost::get<CStealthAddress>(dest);
                std::string sError;
                if (0 != PrepareStealthOutput(sxAddr, scriptPubKey, vStealthData, sError)) {
                    LogPrintf("%s -- PrepareStealthOutput failed. Error = %s\n", __func__, sError);
                    return InvalidAddress;
                }
                fStealthAddress = true;
                CTxDestination newDest;
                if (ExtractDestination(scriptPubKey, newDest))
                    LogPrint("stealth", "%s -- Stealth send to address: %s\n", __func__, CDynamicAddress(newDest).ToString());
            } 
            else {
                scriptPubKey = GetScriptForDestination(dest);
            }
            CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);
            if (fStealthAddress) {
                CScript scriptData;
                scriptData << OP_RETURN << vStealthData;
                CRecipient sendData = {scriptData, 0, fSubtractFeeFromAmount};
                vecSend.push_back(sendData);
            }
            total += rcp.amount;
        }
    }
    if (setAddress.size() != nAddresses) {
        return DuplicateAddress;
    }

    CAmount nBalance = getBalance(&coinControl);

    if (total > nBalance) {
        return AmountExceedsBalance;
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        transaction.newPossibleKeyChange(wallet);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        std::string strFailReason;

        CWalletTx* newTx = transaction.getTransaction();
        CReserveKey* keyChange = transaction.getPossibleKeyChange();

        if (recipients[0].fUseInstantSend && total > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE) * COIN) {
            Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending values that high yet. Transactions are currently limited to %1 DYN.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        bool fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl, true, recipients[0].inputType, recipients[0].fUseInstantSend);
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && fCreated)
            transaction.reassignAmounts(nChangePosRet);

        if (recipients[0].fUseInstantSend) {
            if (newTx->tx->GetValueOut() > sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE) * COIN) {
                Q_EMIT message(tr("Send Coins"), tr("InstantSend doesn't support sending values that high yet. Transactions are currently limited to %1 DYN.").arg(sporkManager.GetSporkValue(SPORK_5_INSTANTSEND_MAX_VALUE)),
                    CClientUIInterface::MSG_ERROR);
                return TransactionCreationFailed;
            }
            if (newTx->tx->vin.size() > CTxLockRequest::WARN_MANY_INPUTS) {
                Q_EMIT message(tr("Send Coins"), tr("Used way too many inputs (>%1) for this InstantSend transaction, fees could be huge.").arg(CTxLockRequest::WARN_MANY_INPUTS),
                    CClientUIInterface::MSG_WARNING);
            }
        }

        if (!fCreated) {
            if (!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance) {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(strFailReason),
                CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // reject absurdly high fee. (This can never happen because the
        // wallet caps the fee at maxTxFee. This merely serves as a
        // belt-and-suspenders check)
        if (nFeeRequired > maxTxFee)
            return AbsurdFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction& transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx* newTx = transaction.getTransaction();
        QList<SendCoinsRecipient> recipients = transaction.getRecipients();

        Q_FOREACH (const SendCoinsRecipient& rcp, recipients) {
            if (rcp.paymentRequest.IsInitialized()) {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(make_pair(key, value));
            } else if (!rcp.message.isEmpty()) // Message from normal dynamic:URI (dynamic:XyZ...?message=example)
            {
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
            }
        }

        CReserveKey* keyChange = transaction.getPossibleKeyChange();
        CValidationState state;
        if (!wallet->CommitTransaction(*newTx, *keyChange, g_connman.get(), state, recipients[0].fUseInstantSend ? NetMsgType::TXLOCKREQUEST : NetMsgType::TX))
            return SendCoinsReturn(TransactionCommitFailed, QString::fromStdString(state.GetRejectReason()));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH (const SendCoinsRecipient& rcp, transaction.getRecipients()) {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized()) {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = CDynamicAddress(strAddress).Get();
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end()) {
                    wallet->SetAddressBook(dest, strLabel, "send");
                } else if (mi->second.name != strLabel) {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendAssets(CWalletTx& tx, QList<SendAssetsRecipient>& recipients, CReserveKey& reservekey)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);

        std::pair<int, std::string> error;
        std::string txid;
        if (!SendAssetTransaction(this->wallet, tx, reservekey, error, txid))
            return SendCoinsReturn(TransactionCommitFailed, QString::fromStdString(error.second));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx.tx;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendAssetsRecipient &rcp : recipients)
    {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized())
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end())
                {
                    wallet->SetAddressBook(dest, strLabel, "send");
                }
                else if (mi->second.name != strLabel)
                {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT assetsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

OptionsModel* WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel* WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

TransactionTableModel* WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

AssetTableModel *WalletModel::getAssetTableModel()
{
    return assetTableModel;
}

MyRestrictedAssetsTableModel *WalletModel::getMyRestrictedAssetsTableModel()
{
    return myRestrictedAssetsTableModel;
}

RecentRequestsTableModel* WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if (!wallet->IsCrypted()) {
        return Unencrypted;
    } else if (wallet->IsLocked(true)) {
        return Locked;
    } else if (wallet->IsLocked()) {
        return UnlockedForMixingOnly;
    } else {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString& passphrase)
{
    if (encrypted) {
        // Encrypt
        Q_EMIT message(tr("Encrypting your wallet..."), tr("This will take just a few seconds."), CClientUIInterface::MSG_INFORMATION);
        return wallet->EncryptWallet(passphrase);
    } else {
        // Decrypt -- TODO; not supported yet
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString& passPhrase, int64_t nSeconds, bool fMixing)
{
    if(locked)
    {
        // Lock
        return wallet->Lock(fMixing);
    }
    else
    {
        // Unlock
        if (!wallet->Unlock(passPhrase))
            return false;

        fWalletUnlockMixStakeOnly = fMixing;

        if (nSeconds > 0)  // seconds
            relockWalletAfterDuration(wallet, nSeconds);

        return true;
    }
}

bool WalletModel::changePassphrase(const SecureString& oldPass, const SecureString& newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString& filename)
{
    return wallet->BackupWallet(filename.toLocal8Bit().data());
}

// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel* walletmodel, CCryptoKeyStore* wallet)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel* walletmodel, CWallet* wallet, const CTxDestination& address, const std::string& label, bool isMine, const std::string& purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(CDynamicAddress(address).ToString());
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
        Q_ARG(QString, strAddress),
        Q_ARG(QString, strLabel),
        Q_ARG(bool, isMine),
        Q_ARG(QString, strPurpose),
        Q_ARG(int, status));
}

static void NotifyTransactionChanged(WalletModel* walletmodel, CWallet* wallet, const uint256& hash, ChangeType status)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    Q_UNUSED(status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
}

static void NotifyMyRestrictedAssetChanged(WalletModel *walletmodel, CWallet *wallet, const std::string &address, const std::string& asset_name,  int type, uint32_t date)
{
    Q_UNUSED(wallet);
    Q_UNUSED(address);
    Q_UNUSED(asset_name);
    Q_UNUSED(type);
    Q_UNUSED(date);
    QMetaObject::invokeMethod(walletmodel, "updateMyRestrictedAssets", Qt::QueuedConnection);
}

static void ShowProgress(WalletModel* walletmodel, const std::string& title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(title)),
        Q_ARG(int, nProgress));
}

static void NotifyWatchonlyChanged(WalletModel* walletmodel, bool fHaveWatchonly)
{
    QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
        Q_ARG(bool, fHaveWatchonly));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyStatusChanged.connect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.connect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->NotifyMyRestrictedAssetsChanged.connect(boost::bind(NotifyMyRestrictedAssetChanged, this, _1, _2, _3, _4, _5));
    wallet->ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.connect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyStatusChanged.disconnect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyAddressBookChanged.disconnect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->NotifyMyRestrictedAssetsChanged.disconnect(boost::bind(NotifyMyRestrictedAssetChanged, this, _1, _2, _3, _4, _5));
    wallet->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.disconnect(boost::bind(NotifyWatchonlyChanged, this, _1));
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock(bool fForMixingOnly)
{
    EncryptionStatus encStatusOld = getEncryptionStatus();

    // Wallet was completely locked
    bool was_locked = (encStatusOld == Locked);
    // Wallet was unlocked for mixing
    bool was_mixing = (encStatusOld == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing and now user requested to fully unlock it
    bool fMixingToFullRequested = !fForMixingOnly && was_mixing;

    if (was_locked || fMixingToFullRequested) {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock(fForMixingOnly);
    }

    EncryptionStatus encStatusNew = getEncryptionStatus();

    // Wallet was locked, user requested to unlock it for mixing and failed to do so
    bool fMixingUnlockFailed = fForMixingOnly && !(encStatusNew == UnlockedForMixingOnly);
    // Wallet was unlocked for mixing, user requested to fully unlock it and failed
    bool fMixingToFullFailed = fMixingToFullRequested && !(encStatusNew == Unlocked);
    // If wallet is still locked, unlock failed or was cancelled, mark context as invalid
    bool fInvalid = (encStatusNew == Locked) || fMixingUnlockFailed || fMixingToFullFailed;
    // Wallet was not locked in any way or user tried to unlock it for mixing only and succeeded, keep it unlocked
    bool fKeepUnlocked = !was_locked || (fForMixingOnly && !fMixingUnlockFailed);

    return UnlockContext(this, !fInvalid, !fKeepUnlocked, was_mixing);
}

WalletModel::UnlockContext::UnlockContext(WalletModel* _wallet, bool _valid, bool _was_locked, bool _was_mixing) : wallet(_wallet),
                                                                                                                   valid(_valid),
                                                                                                                   was_locked(_was_locked),
                                                                                                                   was_mixing(_was_mixing)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if (valid && (was_locked || was_mixing)) {
        wallet->setWalletLocked(true, "", was_mixing);
    }
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.was_locked = false;
    rhs.was_mixing = false;
}

bool WalletModel::getPubKey(const CKeyID& address, CPubKey& vchPubKeyOut) const
{
    return wallet->GetPubKey(address, vchPubKeyOut);
}

bool WalletModel::IsSpendable(const CTxDestination& dest) const
{
    return IsMine(*wallet, dest) & ISMINE_SPENDABLE;
}

bool WalletModel::havePrivKey(const CKeyID& address) const
{
    return wallet->HaveKey(address);
}

bool WalletModel::getPrivKey(const CKeyID& address, CKey& vchPrivKeyOut) const
{
    return wallet->GetKey(address, vchPrivKeyOut);
}

// returns a list of COutputs from COutPoints
void WalletModel::getOutputs(const std::vector<COutPoint>& vOutpoints, std::vector<COutput>& vOutputs)
{
    LOCK2(cs_main, wallet->cs_wallet);
    for (const COutPoint& outpoint : vOutpoints)
    {
        auto it = wallet->mapWallet.find(outpoint.hash);
        if (it == wallet->mapWallet.end()) continue;
        int nDepth = it->second.GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&it->second, outpoint.n, nDepth, true /* spendable */, true /* solvable */, true /* safe */);
        vOutputs.push_back(out);
    }
}

bool WalletModel::isSpent(const COutPoint& outpoint) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsSpent(outpoint.hash, outpoint.n);
}

// AvailableCoins + LockedCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listCoins(std::map<QString, std::vector<COutput> >& mapCoins) const
{
    for (auto& group : wallet->ListCoins()) {
        auto& resultGroup = mapCoins[QString::fromStdString(EncodeDestination(group.first))];
        for (auto& coin : group.second) {
            resultGroup.emplace_back(std::move(coin));
        }
    }
}

/** ASSET START */
// AvailableCoins + LockedCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listAssets(std::map<QString, std::map<QString, std::vector<COutput> > >& mapCoins) const
{
    std::map<QString, std::map<QString, std::vector<COutput> > > mapSortedByAssetName;
    auto list = wallet->ListAssets();

    for (auto& group : list) {
        auto address = QString::fromStdString(EncodeDestination(group.first));

        for (auto& coin : group.second) {
            auto out = coin.tx->tx->vout[coin.i];
            std::string strAssetName;
            CAmount nAmount;
            if (!GetAssetInfoFromScript(out.scriptPubKey, strAssetName, nAmount))
                continue;

            if (nAmount == 0)
                continue;

            QString assetName = QString::fromStdString(strAssetName);
            auto& assetMap = mapCoins[assetName];
            assetMap[address].emplace_back(coin);
        }
    }
}
/** ASSET END */

bool WalletModel::isLockedCoin(uint256 hash, unsigned int n) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsLockedCoin(hash, n);
}

void WalletModel::lockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->LockCoin(output);
}

void WalletModel::unlockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->UnlockCoin(output);
}

void WalletModel::listLockedCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->ListLockedCoins(vOutpts);
}

void WalletModel::loadReceiveRequests(std::vector<std::string>& vReceiveRequests)
{
    LOCK(wallet->cs_wallet);
    BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & item, wallet->mapAddressBook)
        BOOST_FOREACH (const PAIRTYPE(std::string, std::string) & item2, item.second.destdata)
            if (item2.first.size() > 2 && item2.first.substr(0, 2) == "rr") // receive request
                vReceiveRequests.push_back(item2.second);
}

bool WalletModel::saveReceiveRequest(const std::string& sAddress, const int64_t nId, const std::string& sRequest)
{
    CTxDestination dest = CDynamicAddress(sAddress).Get();

    std::stringstream ss;
    ss << nId;
    std::string key = "rr" + ss.str(); // "rr" prefix = "receive request" in destdata

    LOCK(wallet->cs_wallet);
    if (sRequest.empty())
        return wallet->EraseDestData(dest, key);
    else
        return wallet->AddDestData(dest, key, sRequest);
}

bool WalletModel::transactionCanBeAbandoned(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    const CWalletTx* wtx = wallet->GetWalletTx(hash);
    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0 || wtx->IsLockedByInstantSend() || wtx->InMempool())
        return false;
    return true;
}

bool WalletModel::abandonTransaction(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->AbandonTransaction(hash);
}

bool WalletModel::transactionCanBeBumped(uint256 hash) const
{
    return false;
    // For now, remove the ability to bump a transaction. Always return false.
//    LOCK2(cs_main, wallet->cs_wallet);
//    const CWalletTx *wtx = wallet->GetWalletTx(hash);
//    return wtx && SignalsOptInRBF(*wtx) && !wtx->mapValue.count("replaced_by_txid");
}

bool WalletModel::bumpFee(uint256 hash)
{
    return false;
//    std::unique_ptr<CFeeBumper> feeBump;
//    {
//        CCoinControl coinControl;
//        coinControl.signalRbf = true;
//        LOCK2(cs_main, wallet->cs_wallet);
//        feeBump.reset(new CFeeBumper(wallet, hash, coinControl, 0));
//    }
//    if (feeBump->getResult() != BumpFeeResult::OK)
//    {
//        QMessageBox::critical(0, tr("Fee bump error"), tr("Increasing transaction fee failed") + "<br />(" +
//            (feeBump->getErrors().size() ? QString::fromStdString(feeBump->getErrors()[0]) : "") +")");
//         return false;
//    }
//
//    // allow a user based fee verification
//    QString questionString = tr("Do you want to increase the fee?");
//    questionString.append("<br />");
//    CAmount oldFee = feeBump->getOldFee();
//    CAmount newFee = feeBump->getNewFee();
//    questionString.append("<table style=\"text-align: left;\">");
//    questionString.append("<tr><td>");
//    questionString.append(tr("Current fee:"));
//    questionString.append("</td><td>");
//    questionString.append(DynamicUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), oldFee));
//    questionString.append("</td></tr><tr><td>");
//    questionString.append(tr("Increase:"));
//    questionString.append("</td><td>");
//    questionString.append(DynamicUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), newFee - oldFee));
//    questionString.append("</td></tr><tr><td>");
//    questionString.append(tr("New fee:"));
//    questionString.append("</td><td>");
//    questionString.append(DynamicUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(), newFee));
//    questionString.append("</td></tr></table>");
//    SendConfirmationDialog confirmationDialog(tr("Confirm fee bump"), questionString);
//    confirmationDialog.exec();
//    QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();
//
//    // cancel sign&broadcast if users doesn't want to bump the fee
//    if (retval != QMessageBox::Yes) {
//        return false;
//    }
//
//    WalletModel::UnlockContext ctx(requestUnlock());
//    if(!ctx.isValid())
//    {
//        return false;
//    }
//
//    // sign bumped transaction
//    bool res = false;
//    {
//        LOCK2(cs_main, wallet->cs_wallet);
//        res = feeBump->signTransaction(wallet);
//    }
//    if (!res) {
//        QMessageBox::critical(0, tr("Fee bump error"), tr("Can't sign transaction."));
//        return false;
//    }
//    // commit the bumped transaction
//    {
//        LOCK2(cs_main, wallet->cs_wallet);
//        res = feeBump->commit(wallet);
//    }
//    if(!res) {
//        QMessageBox::critical(0, tr("Fee bump error"), tr("Could not commit transaction") + "<br />(" +
//            QString::fromStdString(feeBump->getErrors()[0])+")");
//         return false;
//    }
//    return true;
}

bool WalletModel::isWalletEnabled()
{
    return !gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
}

bool WalletModel::hdEnabled() const
{
    return wallet->IsHDEnabled();
}

CWallet* WalletModel::getWallet()
{
    return wallet;
}

int WalletModel::getDefaultConfirmTarget() const
{
    return nTxConfirmTarget;
}
