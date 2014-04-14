// Copyright (c) 2012-2013 PPCoin developers
// Copyright (c) 2013 Primecoin developers
//
// Introduction
//
// The synchronized checkpoint system is first developed by Sunny King for
// ppcoin network in 2012, giving cryptocurrency developers a tool to gain
// additional network protection against 51% attack.
//
// Primecoin also adopts this security mechanism, and the enforcement of
// checkpoints is explicitly granted by user, thus granting only temporary
// consensual central control to developer at the threats of 51% attack.
//
// Concepts
//
// In the network there can be a privileged node known as 'checkpoint master'.
// This node can send out checkpoint messages signed by the checkpoint master
// key. Each checkpoint is a block hash, representing a block on the blockchain
// that the network should reach consensus on.
//
// Besides verifying signatures of checkpoint messages, each node also verifies
// the consistency of the checkpoints. If a conflicting checkpoint is received,
// it means either the checkpoint master key is compromised, or there is an
// operator mistake. In this situation the node would discard the conflicting
// checkpoint message and display a warning message. This precaution controls
// the damage to network caused by operator mistake or compromised key.
//
// Operations
//
// Checkpoint master key can be established by using the 'makekeypair' command
// The public key in source code should then be updated and private key kept
// in a safe place.
//
// Any node can be turned into checkpoint master by setting the 'checkpointkey'
// configuration parameter with the private key of the checkpoint master key.
// Operator should exercise caution such that at any moment there is at most
// one node operating as checkpoint master. When switching master node, the
// recommended procedure is to shutdown the master node and restart as
// regular node, note down the current checkpoint by 'getcheckpoint', then
// compare to the checkpoint at the new node to be upgraded to master node.
// When the checkpoint on both nodes match then it is safe to switch the new
// node to checkpoint master.
//
// The configuration parameter 'checkpointdepth' specifies how many blocks
// should the checkpoints lag behind the latest block in auto checkpoint mode.
// A depth of 0 is the strongest auto checkpoint policy and offers the greatest
// protection against 51% attack. A negative depth means that the checkpoints
// should not be automatically generated by the checkpoint master, but instead
// be manually entered by operator via the 'sendcheckpoint' command. The manual
// mode is also the default mode (default value -1 for checkpointdepth).
//
// Command 'enforcecheckpoint' and configuration parameter 'checkpointenforce'
// are for the users to explicitly consent to enforce the checkpoints issued
// from checkpoint master. To enforce checkpoint, user needs to either issue
// command 'enforcecheckpoint true', or set configuration parameter
// checkpointenforce=1. The current enforcement setting can be queried via
// command 'getcheckpoint', where 'subscribemode' displays either 'enforce'
// or 'advisory'. The 'enforce' mode of subscribemode means checkpoints are
// enforced. The 'advisory' mode of subscribemode means checkpoints are not
// enforced but a warning message would be displayed if the node is on a 
// different blockchain fork from the checkpoint, and this is the default mode.
//

#include <boost/foreach.hpp>

#include "checkpoints.h"
#include "checkpointsync.h"

#include "base58.h"
#include "bitcoinrpc.h"
#include "main.h"
#include "db.h"
#include "uint256.h"

using namespace json_spirit;
using namespace std;


// ppcoin: sync-checkpoint master key
const std::string CSyncCheckpoint::strMainPubKey = "04b8df82adbe1b2d0f955d8055b9665ea73c167f90013d5c5d90e96e61473b9006104d7c465171945142f2c027290423f6c61cce10eeb1a2399d3bb830e2445d96";
const std::string CSyncCheckpoint::strTestPubKey = "048d6a47c6a67a42dcce44ade4241b57008fe8f57241112a9271eed93fc06dcf117ff2e21296f0f3f76cd38e49d59071891bfd0a9656341bb7692f69a84d808a31";
std::string CSyncCheckpoint::strMasterPrivKey = "";


// ppcoin: synchronized checkpoint (centrally broadcasted)
uint256 hashSyncCheckpoint = 0;
uint256 hashPendingCheckpoint = 0;
CSyncCheckpoint checkpointMessage;
CSyncCheckpoint checkpointMessagePending;
uint256 hashInvalidCheckpoint = 0;
CCriticalSection cs_hashSyncCheckpoint;
std::string strCheckpointWarning;

// ppcoin: get last synchronized checkpoint
CBlockIndex* GetLastSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    if (!mapBlockIndex.count(hashSyncCheckpoint))
        error("GetSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString().c_str());
    else
        return mapBlockIndex[hashSyncCheckpoint];
    return NULL;
}

// ppcoin: only descendant of current sync-checkpoint is allowed
bool ValidateSyncCheckpoint(uint256 hashCheckpoint)
{
    if (!mapBlockIndex.count(hashSyncCheckpoint))
        return error("ValidateSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString().c_str());
    if (!mapBlockIndex.count(hashCheckpoint))
        return error("ValidateSyncCheckpoint: block index missing for received sync-checkpoint %s", hashCheckpoint.ToString().c_str());

    CBlockIndex* pindexSyncCheckpoint = mapBlockIndex[hashSyncCheckpoint];
    CBlockIndex* pindexCheckpointRecv = mapBlockIndex[hashCheckpoint];

    if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
    {
        // Received an older checkpoint, trace back from current checkpoint
        // to the same height of the received checkpoint to verify
        // that current checkpoint should be a descendant block
        CBlockIndex* pindex = pindexSyncCheckpoint;
        while (pindex->nHeight > pindexCheckpointRecv->nHeight)
            if (!(pindex = pindex->pprev))
                return error("ValidateSyncCheckpoint: pprev1 null - block index structure failure");
        if (pindex->GetBlockHash() != hashCheckpoint)
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("ValidateSyncCheckpoint: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
        }
        return false; // ignore older checkpoint
    }

    // Received checkpoint should be a descendant block of the current
    // checkpoint. Trace back to the same height of current checkpoint
    // to verify.
    CBlockIndex* pindex = pindexCheckpointRecv;
    while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
        if (!(pindex = pindex->pprev))
            return error("ValidateSyncCheckpoint: pprev2 null - block index structure failure");
    if (pindex->GetBlockHash() != hashSyncCheckpoint)
    {
        hashInvalidCheckpoint = hashCheckpoint;
        return error("ValidateSyncCheckpoint: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
    }
    return true;
}

bool WriteSyncCheckpoint(const uint256& hashCheckpoint)
{
    CTxDB txdb;
    txdb.TxnBegin();
    if (!txdb.WriteSyncCheckpoint(hashCheckpoint))
    {
        txdb.TxnAbort();
        return error("WriteSyncCheckpoint(): failed to write to txdb sync checkpoint %s", hashCheckpoint.ToString().c_str());
    }
    if (!txdb.TxnCommit())
        return error("WriteSyncCheckpoint(): failed to commit to txdb sync checkpoint %s", hashCheckpoint.ToString().c_str());
    txdb.Close();

    hashSyncCheckpoint = hashCheckpoint;
    return true;
}

bool IsSyncCheckpointEnforced()
{
    return (GetBoolArg("-checkpointenforce", true) || mapArgs.count("-checkpointkey")); // checkpoint master node is always enforced
}

bool AcceptPendingSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    if (hashPendingCheckpoint != 0 && mapBlockIndex.count(hashPendingCheckpoint))
    {
        if (!ValidateSyncCheckpoint(hashPendingCheckpoint))
        {
            hashPendingCheckpoint = 0;
            checkpointMessagePending.SetNull();
            return false;
        }

        CTxDB txdb;
        CBlockIndex* pindexCheckpoint = mapBlockIndex[hashPendingCheckpoint];
        if (IsSyncCheckpointEnforced() && !pindexCheckpoint->IsInMainChain())
        {
            CBlock block;
            if (!block.ReadFromDisk(pindexCheckpoint))
                return error("AcceptPendingSyncCheckpoint: ReadFromDisk failed for sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());
            if (!block.SetBestChain(txdb, pindexCheckpoint))
            {
                hashInvalidCheckpoint = hashPendingCheckpoint;
                return error("AcceptPendingSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());
            }
        }
        txdb.Close();

        if (!WriteSyncCheckpoint(hashPendingCheckpoint))
            return error("AcceptPendingSyncCheckpoint(): failed to write sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());
        hashPendingCheckpoint = 0;
        checkpointMessage = checkpointMessagePending;
        checkpointMessagePending.SetNull();
        printf("AcceptPendingSyncCheckpoint : sync-checkpoint at %s\n", hashSyncCheckpoint.ToString().c_str());
        // relay the checkpoint
        if (!checkpointMessage.IsNull())
        {
            BOOST_FOREACH(CNode* pnode, vNodes)
                checkpointMessage.RelayTo(pnode);
        }
        return true;
    }
    return false;
}

// Automatically select a suitable sync-checkpoint 
uint256 AutoSelectSyncCheckpoint()
{
    // Search backward for a block with specified depth policy
    const CBlockIndex *pindex = pindexBest;
    while (pindex->pprev && pindex->nHeight + (int)GetArg("-checkpointdepth", -1) > pindexBest->nHeight)
        pindex = pindex->pprev;
    return pindex->GetBlockHash();
}

// Check against synchronized checkpoint
bool CheckSyncCheckpoint(const uint256& hashBlock, const CBlockIndex* pindexPrev)
{
    int nHeight = pindexPrev->nHeight + 1;

    LOCK(cs_hashSyncCheckpoint);
    // sync-checkpoint should always be accepted block
    assert(mapBlockIndex.count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];

    if (nHeight > pindexSync->nHeight)
    {
        // trace back to same height as sync-checkpoint
        const CBlockIndex* pindex = pindexPrev;
        while (pindex->nHeight > pindexSync->nHeight)
            if (!(pindex = pindex->pprev))
                return error("CheckSyncCheckpoint: pprev null - block index structure failure");
        if (pindex->nHeight < pindexSync->nHeight || pindex->GetBlockHash() != hashSyncCheckpoint)
            return false; // only descendant of sync-checkpoint can pass check
    }
    if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
        return false; // same height with sync-checkpoint
    if (nHeight < pindexSync->nHeight && !mapBlockIndex.count(hashBlock))
        return false; // lower height than sync-checkpoint
    return true;
}

bool WantedByPendingSyncCheckpoint(uint256 hashBlock)
{
    LOCK(cs_hashSyncCheckpoint);
    if (hashPendingCheckpoint == 0)
        return false;
    if (hashBlock == hashPendingCheckpoint)
        return true;
    if (mapOrphanBlocks.count(hashPendingCheckpoint)
        && hashBlock == WantedByOrphan(mapOrphanBlocks[hashPendingCheckpoint]))
        return true;
    return false;
}

// ppcoin: reset synchronized checkpoint to last hardened checkpoint
bool ResetSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    const uint256& hash = Checkpoints::GetLatestHardenedCheckpoint();
    if (mapBlockIndex.count(hash) && !mapBlockIndex[hash]->IsInMainChain())
    {
        // checkpoint block accepted but not yet in main chain
        printf("ResetSyncCheckpoint: SetBestChain to hardened checkpoint %s\n", hash.ToString().c_str());
        CTxDB txdb;
        CBlock block;
        if (!block.ReadFromDisk(mapBlockIndex[hash]))
            return error("ResetSyncCheckpoint: ReadFromDisk failed for hardened checkpoint %s", hash.ToString().c_str());
        if (!block.SetBestChain(txdb, mapBlockIndex[hash]))
        {
            return error("ResetSyncCheckpoint: SetBestChain failed for hardened checkpoint %s", hash.ToString().c_str());
        }
        txdb.Close();
    }
    else if(!mapBlockIndex.count(hash))
    {
        // checkpoint block not yet accepted
        hashPendingCheckpoint = hash;
        checkpointMessagePending.SetNull();
        printf("ResetSyncCheckpoint: pending for sync-checkpoint %s\n", hashPendingCheckpoint.ToString().c_str());
    }

    if (!WriteSyncCheckpoint((mapBlockIndex.count(hash) && mapBlockIndex[hash]->IsInMainChain())? hash : hashGenesisBlock))
        return error("ResetSyncCheckpoint: failed to write sync checkpoint %s", hash.ToString().c_str());
    printf("ResetSyncCheckpoint: sync-checkpoint reset to %s\n", hashSyncCheckpoint.ToString().c_str());
    return true;
}

void AskForPendingSyncCheckpoint(CNode* pfrom)
{
    LOCK(cs_hashSyncCheckpoint);
    if (pfrom && hashPendingCheckpoint != 0 && (!mapBlockIndex.count(hashPendingCheckpoint)) && (!mapOrphanBlocks.count(hashPendingCheckpoint)))
        pfrom->AskFor(CInv(MSG_BLOCK, hashPendingCheckpoint));
}

// Verify sync checkpoint master pubkey and reset sync checkpoint if changed
bool CheckCheckpointPubKey()
{
    CTxDB txdb;
    std::string strPubKey = "";
    std::string strMasterPubKey = fTestNet? CSyncCheckpoint::strTestPubKey : CSyncCheckpoint::strMainPubKey;
    if (!txdb.ReadCheckpointPubKey(strPubKey) || strPubKey != strMasterPubKey)
    {
        // write checkpoint master key to db
        txdb.TxnBegin();
        if (!txdb.WriteCheckpointPubKey(strMasterPubKey))
            return error("CheckCheckpointPubKey() : failed to write new checkpoint master key to db");
        if (!txdb.TxnCommit())
            return error("CheckCheckpointPubKey() : failed to commit new checkpoint master key to db");
        if (!ResetSyncCheckpoint())
            return error("CheckCheckpointPubKey() : failed to reset sync-checkpoint");
    }
    txdb.Close();
    return true;
}

bool SetCheckpointPrivKey(std::string strPrivKey)
{
    // Test signing a sync-checkpoint with genesis block
    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashGenesisBlock;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(strPrivKey))
        return error("SendSyncCheckpoint: Checkpoint master key invalid");
    CKey key;
    bool fCompressed;
    CSecret secret = vchSecret.GetSecret(fCompressed);
    key.SetSecret(secret, fCompressed); // if key is not correct openssl may crash
    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return false;

    // Test signing successful, proceed
    CSyncCheckpoint::strMasterPrivKey = strPrivKey;
    return true;
}

bool SendSyncCheckpoint(uint256 hashCheckpoint)
{
    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashCheckpoint;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

    if (CSyncCheckpoint::strMasterPrivKey.empty())
        return error("SendSyncCheckpoint: Checkpoint master key unavailable.");
    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(CSyncCheckpoint::strMasterPrivKey))
        return error("SendSyncCheckpoint: Checkpoint master key invalid");
    CKey key;
    bool fCompressed;
    CSecret secret = vchSecret.GetSecret(fCompressed);
    key.SetSecret(secret, fCompressed); // if key is not correct openssl may crash
    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return error("SendSyncCheckpoint: Unable to sign checkpoint, check private key?");

    if(!checkpoint.ProcessSyncCheckpoint(NULL))
    {
        printf("WARNING: SendSyncCheckpoint: Failed to process checkpoint.\n");
        return false;
    }

    // Relay checkpoint
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            checkpoint.RelayTo(pnode);
    }
    return true;
}

// Is the sync-checkpoint outside maturity window?
bool IsMatureSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    // sync-checkpoint should always be accepted block
    assert(mapBlockIndex.count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];
    return (nBestHeight >= pindexSync->nHeight + COINBASE_MATURITY);
}

// Is the sync-checkpoint too old?
bool IsSyncCheckpointTooOld(unsigned int nSeconds)
{
    LOCK(cs_hashSyncCheckpoint);
    // sync-checkpoint should always be accepted block
    assert(mapBlockIndex.count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];
    return (pindexSync->GetBlockTime() + nSeconds < GetAdjustedTime());
}

// ppcoin: find block wanted by given orphan block
uint256 WantedByOrphan(const CBlock* pblockOrphan)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblockOrphan->hashPrevBlock))
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrevBlock];
    return pblockOrphan->hashPrevBlock;
}

// ppcoin: verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    CKey key;
    std::string strMasterPubKey = fTestNet? CSyncCheckpoint::strTestPubKey : CSyncCheckpoint::strMainPubKey;
    if (!key.SetPubKey(ParseHex(strMasterPubKey)))
        return error("CSyncCheckpoint::CheckSignature() : SetPubKey failed");
    if (!key.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig))
        return error("CSyncCheckpoint::CheckSignature() : verify signature failed");

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *(CUnsignedSyncCheckpoint*)this;
    return true;
}

// ppcoin: process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint(CNode* pfrom)
{
    if (!CheckSignature())
        return false;

    LOCK(cs_hashSyncCheckpoint);
    if (!mapBlockIndex.count(hashCheckpoint))
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        hashPendingCheckpoint = hashCheckpoint;
        checkpointMessagePending = *this;
        printf("ProcessSyncCheckpoint: pending for sync-checkpoint %s\n", hashCheckpoint.ToString().c_str());
        // Ask this guy to fill in what we're missing
        if (pfrom)
        {
            pfrom->PushGetBlocks(pindexBest, hashCheckpoint);
            // ask directly as well in case rejected earlier by duplicate
            // proof-of-stake because getblocks may not get it this time
            pfrom->AskFor(CInv(MSG_BLOCK, mapOrphanBlocks.count(hashCheckpoint)? WantedByOrphan(mapOrphanBlocks[hashCheckpoint]) : hashCheckpoint));
        }
        return false;
    }

    if (!ValidateSyncCheckpoint(hashCheckpoint))
        return false;

    CTxDB txdb;
    CBlockIndex* pindexCheckpoint = mapBlockIndex[hashCheckpoint];
    if (IsSyncCheckpointEnforced() && !pindexCheckpoint->IsInMainChain())
    {
        // checkpoint chain received but not yet main chain
        CBlock block;
        if (!block.ReadFromDisk(pindexCheckpoint))
            return error("ProcessSyncCheckpoint: ReadFromDisk failed for sync checkpoint %s", hashCheckpoint.ToString().c_str());
        if (!block.SetBestChain(txdb, pindexCheckpoint))
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("ProcessSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashCheckpoint.ToString().c_str());
        }
    }
    txdb.Close();

    if (!WriteSyncCheckpoint(hashCheckpoint))
        return error("ProcessSyncCheckpoint(): failed to write sync checkpoint %s", hashCheckpoint.ToString().c_str());
    checkpointMessage = *this;
    hashPendingCheckpoint = 0;
    checkpointMessagePending.SetNull();
    printf("ProcessSyncCheckpoint: sync-checkpoint at %s\n", hashCheckpoint.ToString().c_str());
    return true;
}


// RPC commands related to sync checkpoints
// get information of sync-checkpoint (first introduced in ppcoin)
Value getcheckpoint(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcheckpoint\n"
            "Show info of synchronized checkpoint.\n");

    Object result;
    CBlockIndex* pindexCheckpoint;

    result.push_back(Pair("synccheckpoint", hashSyncCheckpoint.ToString().c_str()));
    if (mapBlockIndex.count(hashSyncCheckpoint))
    {
        pindexCheckpoint = mapBlockIndex[hashSyncCheckpoint];
        result.push_back(Pair("height", pindexCheckpoint->nHeight));
        result.push_back(Pair("timestamp", (boost::int64_t) pindexCheckpoint->GetBlockTime()));
    }
    result.push_back(Pair("subscribemode", IsSyncCheckpointEnforced()? "enforce" : "advisory"));
    if (mapArgs.count("-checkpointkey"))
        result.push_back(Pair("checkpointmaster", true));

    return result;
}

Value sendcheckpoint(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "sendcheckpoint <blockhash>\n"
            "Send a synchronized checkpoint.\n");

    if (!mapArgs.count("-checkpointkey") || CSyncCheckpoint::strMasterPrivKey.empty())
        throw runtime_error("Not a checkpointmaster node, first set checkpointkey in configuration and restart client. ");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (!SendSyncCheckpoint(hash))
        throw runtime_error("Failed to send checkpoint, check log. ");

    Object result;
    CBlockIndex* pindexCheckpoint;

    result.push_back(Pair("synccheckpoint", hashSyncCheckpoint.ToString().c_str()));
    if (mapBlockIndex.count(hashSyncCheckpoint))
    {
        pindexCheckpoint = mapBlockIndex[hashSyncCheckpoint];
        result.push_back(Pair("height", pindexCheckpoint->nHeight));
        result.push_back(Pair("timestamp", (boost::int64_t) pindexCheckpoint->GetBlockTime()));
    }
    result.push_back(Pair("subscribemode", IsSyncCheckpointEnforced()? "enforce" : "advisory"));
    if (mapArgs.count("-checkpointkey"))
        result.push_back(Pair("checkpointmaster", true));

    return result;
}

Value enforcecheckpoint(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "enforcecheckpoint <enforce>\n"
            "<enforce> is true or false to enable or disable enforcement of broadcasted checkpoints by developer.");

    bool fEnforceCheckpoint = params[0].get_bool();
    if (mapArgs.count("-checkpointkey") && !fEnforceCheckpoint)
        throw runtime_error(
            "checkpoint master node must enforce synchronized checkpoints.");
    if (fEnforceCheckpoint)
        strCheckpointWarning = "";
    mapArgs["-checkpointenforce"] = (fEnforceCheckpoint ? "1" : "0");
    return Value::null;
}
