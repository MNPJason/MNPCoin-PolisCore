// Copyright (c) 2011-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_TEST_MODULE Polis Test Suite

#include "test_polis.h"

#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include "validation.h"
#include "miner.h"
#include "net_processing.h"
#include "pubkey.h"
#include "random.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "rpc/server.h"
#include "rpc/register.h"
#include "script/sigcache.h"
#include "test/testutil.h"
#include "wallet/wallet.h"

#include "evo/specialtx.h"
#include "evo/deterministicmns.h"
#include "evo/cbtx.h"
#include "llmq/quorums_init.h"

#include <memory>

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread.hpp>

std::unique_ptr<CConnman> g_connman;
FastRandomContext insecure_rand_ctx(true);

extern bool fPrintToConsole;
extern void noui_connect();

BasicTestingSetup::BasicTestingSetup(const std::string& chainName)
{
        ECC_Start();
        BLSInit();
        SetupEnvironment();
        SetupNetworking();
        InitSignatureCache();
        fPrintToDebugLog = false; // don't want to write to debug.log file
        fCheckBlockIndex = true;
        SelectParams(chainName);
        evoDb = new CEvoDB(1 << 20, true, true);
        deterministicMNManager = new CDeterministicMNManager(*evoDb);
        noui_connect();
}

BasicTestingSetup::~BasicTestingSetup()
{
        delete deterministicMNManager;
        delete evoDb;

        ECC_Stop();
        g_connman.reset();
}

TestingSetup::TestingSetup(const std::string& chainName) : BasicTestingSetup(chainName)
{
    const CChainParams& chainparams = Params();
        // Ideally we'd move all the RPC tests to the functional testing framework
        // instead of unit tests, but for now we need these here.
        RegisterAllCoreRPCCommands(tableRPC);
        ClearDatadirCache();
        pathTemp = GetTempPath() / strprintf("test_polis_%lu_%i", (unsigned long)GetTime(), (int)(GetRand(100000)));
        boost::filesystem::create_directories(pathTemp);
        ForceSetArg("-datadir", pathTemp.string());
        mempool.setSanityCheck(1.0);
        pblocktree = new CBlockTreeDB(1 << 20, true);
        pcoinsdbview = new CCoinsViewDB(1 << 23, true);
        llmq::InitLLMQSystem(*evoDb);
        pcoinsTip = new CCoinsViewCache(pcoinsdbview);
        InitBlockIndex(chainparams);
        {
            CValidationState state;
            bool ok = ActivateBestChain(state, chainparams);
            BOOST_CHECK(ok);
        }
        nScriptCheckThreads = 3;
        for (int i=0; i < nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
        g_connman = std::unique_ptr<CConnman>(new CConnman(0x1337, 0x1337)); // Deterministic randomness for tests.
        connman = g_connman.get();
        RegisterNodeSignals(GetNodeSignals());
}

TestingSetup::~TestingSetup()
{
        UnregisterNodeSignals(GetNodeSignals());
        threadGroup.interrupt_all();
        threadGroup.join_all();
        UnloadBlockIndex();
        delete pcoinsTip;
        llmq::DestroyLLMQSystem();
        delete pcoinsdbview;
        delete pblocktree;
        boost::filesystem::remove_all(pathTemp);
}

TestChainSetup::TestChainSetup(int blockCount) : TestingSetup(CBaseChainParams::REGTEST)
{
    // Generate a 100-block chain:
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (int i = 0; i < blockCount; i++)
    {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }
}

//
// Create a new block with just given transactions, coinbase paying to
// scriptPubKey, and try to add it to the current chain.
//
CBlock
TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    auto block = CreateBlock(txns, scriptPubKey);

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    ProcessNewBlock(chainparams, shared_pblock, true, NULL);

    CBlock result = block;
    return result;
}

CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    return CreateAndProcessBlock(txns, scriptPubKey);
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(chainparams).CreateNewBlock(pwalletMain, chainparams, scriptPubKey, false);
    CBlock& block = pblocktemplate->block;

    std::vector<CTransactionRef> llmqCommitments;
    for (const auto& tx : block.vtx) {
        if (tx->nVersion == 3 && tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            llmqCommitments.emplace_back(tx);
        }
    }

    // Replace mempool-selected txns with just coinbase plus passed-in txns:
    block.vtx.resize(1);
    // Re-add quorum commitments
    block.vtx.insert(block.vtx.end(), llmqCommitments.begin(), llmqCommitments.end());
    BOOST_FOREACH(const CMutableTransaction& tx, txns)
        block.vtx.push_back(MakeTransactionRef(tx));

    // Manually update CbTx as we modified the block here
    if (block.vtx[0]->nType == TRANSACTION_COINBASE) {
        LOCK(cs_main);
        CCbTx cbTx;
        if (!GetTxPayload(*block.vtx[0], cbTx)) {
            BOOST_ASSERT(false);
        }
        CValidationState state;
        if (!CalcCbTxMerkleRootMNList(block, chainActive.Tip(), cbTx.merkleRootMNList, state)) {
            BOOST_ASSERT(false);
        }
        CMutableTransaction tmpTx = *block.vtx[0];
        SetTxPayload(tmpTx, cbTx);
        block.vtx[0] = MakeTransactionRef(tmpTx);
    }

    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    unsigned int extraNonce = 0;
    IncrementExtraNonce(&block, chainActive.Tip(), extraNonce);

    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus())) ++block.nNonce;

    CBlock result = block;
    return result;
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    return CreateBlock(txns, scriptPubKey);
}

TestChainSetup::~TestChainSetup()
{
}


CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction &tx, CTxMemPool *pool) {
    CTransaction txn(tx);
    return FromTx(txn, pool);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction &txn, CTxMemPool *pool) {
    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, dPriority, nHeight,
                           txn.GetValueOut(), spendsCoinbase, sigOpCount, lp);
}

void Shutdown(void* parg)
{
  exit(0);
}

void StartShutdown()
{
  exit(0);
}

bool ShutdownRequested()
{
  return false;
}
