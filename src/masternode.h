// Copyright (c) 2014-2017 The Polis Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include "key.h"
#include "validation.h"
#include "spork.h"
#include "bls/bls.h"

#include "evo/deterministicmns.h"

class CMasternode;
class CMasternodeBroadcast;
class CConnman;

static const int MASTERNODE_CHECK_SECONDS               =   5;
static const int MASTERNODE_MIN_MNB_SECONDS             =   5 * 60;
static const int MASTERNODE_MIN_MNP_SECONDS             =  10 * 60;
static const int MASTERNODE_SENTINEL_PING_MAX_SECONDS   =  60 * 60;
static const int MASTERNODE_EXPIRATION_SECONDS          = 120 * 60;
static const int MASTERNODE_NEW_START_REQUIRED_SECONDS  = 180 * 60;

static const int MASTERNODE_POSE_BAN_MAX_SCORE          = 5;
static const int MASTERNODE_MAX_MIXING_TXES             = 5;

//
// The Masternode Ping Class : Contains a different serialize method for sending pings from masternodes throughout the network
//

// sentinel version before implementation of nSentinelVersion in CMasternodePing
#define DEFAULT_SENTINEL_VERSION 0x010001
// daemon version before implementation of nDaemonVersion in CMasternodePing
#define DEFAULT_DAEMON_VERSION 120200

class CMasternodePing
{
public:
    COutPoint masternodeOutpoint{};
    uint256 blockHash{};
    int64_t sigTime{}; //mnb message times
    std::vector<unsigned char> vchSig{};
    bool fSentinelIsCurrent = false; // true if last sentinel ping was current
    // MSB is always 0, other 3 bits corresponds to x.x.x version scheme
    uint32_t nSentinelVersion{DEFAULT_SENTINEL_VERSION};
    uint32_t nDaemonVersion{DEFAULT_DAEMON_VERSION};

    CMasternodePing() = default;

    CMasternodePing(const COutPoint& outpoint);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(masternodeOutpoint);
        READWRITE(blockHash);
        READWRITE(sigTime);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
        READWRITE(fSentinelIsCurrent);
        READWRITE(nSentinelVersion);
        READWRITE(nDaemonVersion);
    }

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    bool IsExpired() const { return GetAdjustedTime() - sigTime > MASTERNODE_NEW_START_REQUIRED_SECONDS; }

    bool Sign(const CKey& keyMasternode, const CKeyID& keyIDOperator);
    bool CheckSignature(CKeyID& keyIDOperator, int &nDos) const;
    bool SimpleCheck(int& nDos);
    bool CheckAndUpdate(CMasternode* pmn, bool fFromNewBroadcast, int& nDos, CConnman& connman);
    void Relay(CConnman& connman);

    std::string GetSentinelString() const;
    std::string GetDaemonString() const;

    explicit operator bool() const;
};

inline bool operator==(const CMasternodePing& a, const CMasternodePing& b)
{
    return a.masternodeOutpoint == b.masternodeOutpoint && a.blockHash == b.blockHash;
}
inline bool operator!=(const CMasternodePing& a, const CMasternodePing& b)
{
    return !(a == b);
}
inline CMasternodePing::operator bool() const
{
    return *this != CMasternodePing();
}

struct masternode_info_t
{
    // Note: all these constructors can be removed once C++14 is enabled.
    // (in C++11 the member initializers wrongly disqualify this as an aggregate)
    masternode_info_t() = default;
    masternode_info_t(masternode_info_t const&) = default;

    masternode_info_t(int activeState, int protoVer, int64_t sTime) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime} {}

    // only called when the network is in legacy MN list mode
    masternode_info_t(int activeState, int protoVer, int64_t sTime,
                      COutPoint const& outpnt, CService const& addr,
                      CPubKey const& pkCollAddr, CPubKey const& pkMN) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
        outpoint{outpnt}, addr{addr},
        pubKeyCollateralAddress{pkCollAddr}, pubKeyMasternode{pkMN}, keyIDCollateralAddress{pkCollAddr.GetID()}, keyIDOwner{pkMN.GetID()}, legacyKeyIDOperator{pkMN.GetID()}, keyIDVoting{pkMN.GetID()} {}

    // only called when the network is in deterministic MN list mode
    masternode_info_t(int activeState, int protoVer, int64_t sTime,
                      COutPoint const& outpnt, CService const& addr,
                      CKeyID const& pkCollAddr, CKeyID const& pkOwner, CBLSPublicKey const& pkOperator, CKeyID const& pkVoting) :
        nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
        outpoint{outpnt}, addr{addr},
        pubKeyCollateralAddress{}, pubKeyMasternode{}, keyIDCollateralAddress{pkCollAddr}, keyIDOwner{pkOwner}, blsPubKeyOperator{pkOperator}, keyIDVoting{pkVoting} {}

    int nActiveState = 0;
    int nProtocolVersion = 0;
    int64_t sigTime = 0; //mnb message time

    COutPoint outpoint{};
    CService addr{};
    CPubKey pubKeyCollateralAddress{}; // this will be invalid/unset when the network switches to deterministic MNs (luckely it's only important for the broadcast hash)
    CPubKey pubKeyMasternode{}; // this will be invalid/unset when the network switches to deterministic MNs (luckely it's only important for the broadcast hash)
    CKeyID keyIDCollateralAddress{}; // this is only used in compatibility code and won't be used when spork15 gets activated
    CKeyID keyIDOwner{};
    CKeyID legacyKeyIDOperator{};
    CBLSPublicKey blsPubKeyOperator;
    CKeyID keyIDVoting{};

    int64_t nLastDsq = 0; //the dsq count from the last dsq broadcast of this node
    int64_t nTimeLastChecked = 0;
    int64_t nTimeLastPaid = 0;
    int64_t nTimeLastPing = 0; //* not in CMN
    bool fInfoValid = false; //* not in CMN
};

//
// The Masternode Class. For managing the Darksend process. It contains the input of the 1000DRK, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CMasternode : public masternode_info_t
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    enum state {
        MASTERNODE_PRE_ENABLED,
        MASTERNODE_ENABLED,
        MASTERNODE_EXPIRED,
        MASTERNODE_OUTPOINT_SPENT,
        MASTERNODE_UPDATE_REQUIRED,
        MASTERNODE_SENTINEL_PING_EXPIRED,
        MASTERNODE_NEW_START_REQUIRED,
        MASTERNODE_POSE_BAN
    };

    enum CollateralStatus {
        COLLATERAL_OK,
        COLLATERAL_UTXO_NOT_FOUND,
        COLLATERAL_INVALID_AMOUNT,
        COLLATERAL_INVALID_PUBKEY,
    };


    CMasternodePing lastPing{};
    std::vector<unsigned char> vchSig{};

    uint256 nCollateralMinConfBlockHash{};
    int nBlockLastPaid{};
    int nPoSeBanScore{};
    int nPoSeBanHeight{};
    int nMixingTxCount{};
    bool fUnitTest = false;

    // KEEP TRACK OF GOVERNANCE ITEMS EACH MASTERNODE HAS VOTE UPON FOR RECALCULATION
    std::map<uint256, int> mapGovernanceObjectsVotedOn;

    CMasternode();
    CMasternode(const CMasternode& other);
    CMasternode(const CMasternodeBroadcast& mnb);
    CMasternode(CService addrNew, COutPoint outpointNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int nProtocolVersionIn);
    CMasternode(const uint256 &proTxHash, const CDeterministicMNCPtr& dmn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        READWRITE(outpoint);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyMasternode);
        READWRITE(keyIDCollateralAddress);
        READWRITE(keyIDOwner);
        READWRITE(legacyKeyIDOperator);
        READWRITE(blsPubKeyOperator);
        READWRITE(keyIDVoting);
        READWRITE(lastPing);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nLastDsq);
        READWRITE(nTimeLastChecked);
        READWRITE(nTimeLastPaid);
        READWRITE(nActiveState);
        READWRITE(nCollateralMinConfBlockHash);
        READWRITE(nBlockLastPaid);
        READWRITE(nProtocolVersion);
        READWRITE(nPoSeBanScore);
        READWRITE(nPoSeBanHeight);
        READWRITE(nMixingTxCount);
        READWRITE(fUnitTest);
        READWRITE(mapGovernanceObjectsVotedOn);
    }

    // CALCULATE A RANK AGAINST OF GIVEN BLOCK
    arith_uint256 CalculateScore(const uint256& blockHash) const;

    bool UpdateFromNewBroadcast(CMasternodeBroadcast& mnb, CConnman& connman);

    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CKeyID& keyID);
    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CKeyID& keyID, int& nHeightRet);
    void Check(bool fForce = false);

    bool IsBroadcastedWithin(int nSeconds) { return GetAdjustedTime() - sigTime < nSeconds; }

    bool IsPingedWithin(int nSeconds, int64_t nTimeToCheckAt = -1)
    {
        if(!lastPing) return false;

        if(nTimeToCheckAt == -1) {
            nTimeToCheckAt = GetAdjustedTime();
        }
        return nTimeToCheckAt - lastPing.sigTime < nSeconds;
    }

    bool IsEnabled() const { return nActiveState == MASTERNODE_ENABLED; }
    bool IsPreEnabled() const { return nActiveState == MASTERNODE_PRE_ENABLED; }
    bool IsPoSeBanned() const { return nActiveState == MASTERNODE_POSE_BAN; }
    // NOTE: this one relies on nPoSeBanScore, not on nActiveState as everything else here
    bool IsPoSeVerified() const { return nPoSeBanScore <= -MASTERNODE_POSE_BAN_MAX_SCORE; }
    bool IsExpired() const { return nActiveState == MASTERNODE_EXPIRED; }
    bool IsOutpointSpent() const { return nActiveState == MASTERNODE_OUTPOINT_SPENT; }
    bool IsUpdateRequired() const { return nActiveState == MASTERNODE_UPDATE_REQUIRED; }
    bool IsSentinelPingExpired() const { return nActiveState == MASTERNODE_SENTINEL_PING_EXPIRED; }
    bool IsNewStartRequired() const { return nActiveState == MASTERNODE_NEW_START_REQUIRED; }

    static bool IsValidStateForAutoStart(int nActiveStateIn)
    {
        return  nActiveStateIn == MASTERNODE_ENABLED ||
                nActiveStateIn == MASTERNODE_PRE_ENABLED ||
                nActiveStateIn == MASTERNODE_EXPIRED ||
                nActiveStateIn == MASTERNODE_SENTINEL_PING_EXPIRED;
    }

    bool IsValidForPayment() const
    {
        if(nActiveState == MASTERNODE_ENABLED) {
            return true;
        }
        if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
           (nActiveState == MASTERNODE_SENTINEL_PING_EXPIRED)) {
            return true;
        }

        return false;
    }

    bool IsValidForMixingTxes() const
    {
        return nMixingTxCount <= MASTERNODE_MAX_MIXING_TXES;
    }

    bool IsValidNetAddr();
    static bool IsValidNetAddr(CService addrIn);

    void IncreasePoSeBanScore() { if(nPoSeBanScore < MASTERNODE_POSE_BAN_MAX_SCORE) nPoSeBanScore++; }
    void DecreasePoSeBanScore() { if(nPoSeBanScore > -MASTERNODE_POSE_BAN_MAX_SCORE) nPoSeBanScore--; }
    void PoSeBan() { nPoSeBanScore = MASTERNODE_POSE_BAN_MAX_SCORE; }

    masternode_info_t GetInfo() const;

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;

    int GetLastPaidTime() const { return nTimeLastPaid; }
    int GetLastPaidBlock() const { return nBlockLastPaid; }
    void UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack);

    // KEEP TRACK OF EACH GOVERNANCE ITEM INCASE THIS NODE GOES OFFLINE, SO WE CAN RECALC THEIR STATUS
    void AddGovernanceVote(uint256 nGovernanceObjectHash);
    // RECALCULATE CACHED STATUS FLAGS FOR ALL AFFECTED OBJECTS
    void FlagGovernanceItemsAsDirty();

    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    CMasternode& operator=(CMasternode const& from)
    {
        static_cast<masternode_info_t&>(*this)=from;
        lastPing = from.lastPing;
        vchSig = from.vchSig;
        nCollateralMinConfBlockHash = from.nCollateralMinConfBlockHash;
        nBlockLastPaid = from.nBlockLastPaid;
        nPoSeBanScore = from.nPoSeBanScore;
        nPoSeBanHeight = from.nPoSeBanHeight;
        nMixingTxCount = from.nMixingTxCount;
        fUnitTest = from.fUnitTest;
        mapGovernanceObjectsVotedOn = from.mapGovernanceObjectsVotedOn;
        return *this;
    }
};

inline bool operator==(const CMasternode& a, const CMasternode& b)
{
    return a.outpoint == b.outpoint;
}
inline bool operator!=(const CMasternode& a, const CMasternode& b)
{
    return !(a.outpoint == b.outpoint);
}


//
// The Masternode Broadcast Class : Contains a different serialize method for sending masternodes through the network
//

class CMasternodeBroadcast : public CMasternode
{
public:

    bool fRecovery;

    CMasternodeBroadcast() : CMasternode(), fRecovery(false) {}
    CMasternodeBroadcast(const CMasternode& mn) : CMasternode(mn), fRecovery(false) {}
    CMasternodeBroadcast(CService addrNew, COutPoint outpointNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int nProtocolVersionIn) :
        CMasternode(addrNew, outpointNew, pubKeyCollateralAddressNew, pubKeyMasternodeNew, nProtocolVersionIn), fRecovery(false) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(outpoint);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyMasternode);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
        READWRITE(sigTime);
        READWRITE(nProtocolVersion);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(lastPing);
        }

        if (ser_action.ForRead()) {
            keyIDCollateralAddress = pubKeyCollateralAddress.GetID();
            keyIDOwner = pubKeyMasternode.GetID();
            legacyKeyIDOperator = pubKeyMasternode.GetID();
            keyIDVoting = pubKeyMasternode.GetID();
        }
    }

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    /// Create Masternode broadcast, needs to be relayed manually after that
    static bool Create(const COutPoint& outpoint, const CService& service, const CKey& keyCollateralAddressNew, const CPubKey& pubKeyCollateralAddressNew, const CKey& keyMasternodeNew, const CPubKey& pubKeyMasternodeNew, std::string &strErrorRet, CMasternodeBroadcast &mnbRet);
    static bool Create(const std::string& strService, const std::string& strKey, const std::string& strTxHash, const std::string& strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast &mnbRet, bool fOffline = false);

    bool SimpleCheck(int& nDos);
    bool Update(CMasternode* pmn, int& nDos, CConnman& connman);
    bool CheckOutpoint(int& nDos);

    bool Sign(const CKey& keyCollateralAddress);
    bool CheckSignature(int& nDos) const;
    void Relay(CConnman& connman) const;
};

class CMasternodeVerification
{
public:
    COutPoint masternodeOutpoint1{};
    COutPoint masternodeOutpoint2{};
    CService addr{};
    int nonce{};
    int nBlockHeight{};
    std::vector<unsigned char> vchSig1{};
    std::vector<unsigned char> vchSig2{};

    CMasternodeVerification() = default;

    CMasternodeVerification(CService addr, int nonce, int nBlockHeight) :
        addr(addr),
        nonce(nonce),
        nBlockHeight(nBlockHeight)
    {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(masternodeOutpoint1);
        READWRITE(masternodeOutpoint2);
        READWRITE(addr);
        READWRITE(nonce);
        READWRITE(nBlockHeight);
        READWRITE(vchSig1);
        READWRITE(vchSig2);
    }

    uint256 GetHash() const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        // adding dummy values here to match old hashing format
        ss << masternodeOutpoint1 << uint8_t{} << 0xffffffff;
        ss << masternodeOutpoint2 << uint8_t{} << 0xffffffff;
        ss << addr;
        ss << nonce;
        ss << nBlockHeight;
        return ss.GetHash();
    }

    uint256 GetSignatureHash1(const uint256& blockHash) const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << addr;
        ss << nonce;
        ss << blockHash;
        return ss.GetHash();
    }

    uint256 GetSignatureHash2(const uint256& blockHash) const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << masternodeOutpoint1;
        ss << masternodeOutpoint2;
        ss << addr;
        ss << nonce;
        ss << blockHash;
        return ss.GetHash();
    }

    void Relay() const
    {
        CInv inv(MSG_MASTERNODE_VERIFY, GetHash());
        g_connman->RelayInv(inv);
    }
};

#endif
