// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"

#include "arith_uint256.h"
#include "chainparams.h"
#include "hash.h"
#include "pow.h"
#include "uint256.h"

#include <stdint.h>

#include <boost/thread.hpp>

using namespace std;

static const char DB_COINS = 'c';
static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_BEST_BLOCK = 'B';
static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';


CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe, true) 
{
}

bool CCoinsViewDB::GetCoins(const uint256 &txid, CCoins &coins) const {
    return db.Read(make_pair(DB_COINS, txid), coins);
}

bool CCoinsViewDB::HaveCoins(const uint256 &txid) const {
    return db.Exists(make_pair(DB_COINS, txid));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!db.Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) {
    CDBBatch batch(db);
    size_t count = 0;
    size_t changed = 0;
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            if (it->second.coins.IsPruned())
                batch.Erase(make_pair(DB_COINS, it->first));
            else
                batch.Write(make_pair(DB_COINS, it->first), it->second.coins);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }
    if (!hashBlock.IsNull())
        batch.Write(DB_BEST_BLOCK, hashBlock);

    LogPrint("coindb", "Committing %u changed transactions (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return db.WriteBatch(batch);
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

CCoinsViewCursor *CCoinsViewDB::Cursor() const
{
    CCoinsViewDBCursor *i = new CCoinsViewDBCursor(const_cast<CDBWrapper*>(&db)->NewIterator(), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COINS);
    // Cache key of first record
    i->pcursor->GetKey(i->keyTmp);
    return i;
}

bool CCoinsViewDBCursor::GetKey(uint256 &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COINS) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(CCoins &coins) const
{
    return pcursor->GetValue(coins);
}

unsigned int CCoinsViewDBCursor::GetValueSize() const
{
    return pcursor->GetValueSize();
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COINS;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    if (!pcursor->Valid() || !pcursor->GetKey(keyTmp))
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(make_pair(DB_TXINDEX, txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts(boost::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_BLOCK_INDEX, uint256()));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                pindexNew->baseTarget                  = diskindex.baseTarget;
                pindexNew->generationSignature         = diskindex.generationSignature;
                pindexNew->pubKeyOfpackager            = diskindex.pubKeyOfpackager;
                pindexNew->cumulativeDifficulty        = diskindex.cumulativeDifficulty;
                pindexNew->nChainDiff        = UintToArith256(diskindex.cumulativeDifficulty);

                pcursor->Next();
            } else {
                return error("LoadBlockIndex() : failed to read value");
            }
        } else {
            break;
        }
    }

    return true;
}

CBalanceViewDB::CBalanceViewDB()
{
    options.create_if_missing = true;

    std::string db_path = GetDataDir(true).string() + std::string("/balance");
    LogPrintf("Opening LevelDB in %s\n", db_path);

    leveldb::Status status = leveldb::DB::Open(options, db_path, &pdb);
    dbwrapper_private::HandleError(status);
    assert(status.ok());
    LogPrintf("Opened LevelDB successfully\n");
}

CBalanceViewDB::~CBalanceViewDB()
{
    delete pdb;
    pdb = NULL;
}

bool CBalanceViewDB::WriteDB(std::string key, int nHeight, CAmount value)
{
    std::stringstream ssVal;
    ssVal << value;
    std::string strValue;
    ssVal >> strValue;

    std::stringstream ssHeight;
    std::string strHeight;
    ssHeight << nHeight;
    ssHeight >> strHeight;

    leveldb::Status status = pdb->Put(leveldb::WriteOptions(), key+"_"+strHeight, strValue);
    if(!status.ok())
    {
        LogPrintf("LevelDB write failure in balance module: %s\n", status.ToString());
        dbwrapper_private::HandleError(status);
        return false;
    }

    return true;
}

bool CBalanceViewDB::ReadDB(std::string key, int nHeight, CAmount& value)
{
    std::stringstream ssHeight;
    std::string strHeight;
    ssHeight << nHeight;
    ssHeight >> strHeight;

    std::string strValue;
    leveldb::Status status = pdb->Get(leveldb::ReadOptions(), key+"_"+strHeight, &strValue);
    if(!status.ok())
    {
        if (status.IsNotFound())
            value = 0;
        else
        {
            LogPrintf("LevelDB read failure in balance module: %s\n", status.ToString());
            dbwrapper_private::HandleError(status);
        }
        return false;
    }

    std::istringstream ssVal(strValue);
    ssVal >> value;

    return true;
}

void CBalanceViewDB::ClearCache()
{
    cacheBalance.clear();
}

CAmount CBalanceViewDB::GetBalance(std::string address, int nHeight)
{
    if (cacheBalance.find(address) != cacheBalance.end())
        return cacheBalance[address];
    else
    {
        for (int h = nHeight; h >= 0; h--)
        {
            CAmount amount = 0;
            if (ReadDB(address, h, amount))
                return amount;
        }
    }

    return 0;
}

bool CBalanceViewDB::UpdateBalance(const CTransaction& tx, const CCoinsViewCache& inputs, int nHeight)
{
    if (tx.vout.size() > 0)
    {
        CBitcoinAddress addr;

        if (!tx.IsCoinBase() && tx.vin.size() > 0)
        {
            for(uint i = 0; i < tx.vin.size(); i++)
            {
                const CCoins* coins = inputs.AccessCoins(tx.vin[i].prevout.hash);
                assert(coins);

                std::string address;
                addr.ScriptPub2Addr(coins->vout[tx.vin[i].prevout.n].scriptPubKey, address);

                if (nHeight > 0)
                {
                    CAmount val = GetBalance(address, nHeight - 1);

                    std::cout<<"====="<<address<<":   "<<val<<" - "<<coins->vout[tx.vin[i].prevout.n].nValue;

                    val -= coins->vout[tx.vin[i].prevout.n].nValue;

                    std::cout<<" = "<<val<<std::endl;

                    if (cacheBalance.find(address) == cacheBalance.end())
                        cacheBalance.insert(pair<std::string, CAmount>(address, val));
                    else
                        cacheBalance[address] = val;
                    if (!WriteDB(address, nHeight, val))
                        return false;
                }
            }
        }

        for(uint o = 0; o < tx.vout.size(); o++)
        {
            if (tx.vout[o].nValue <= 0)
                continue;

            std::string address;
            addr.ScriptPub2Addr(tx.vout[o].scriptPubKey, address);

            CAmount val = GetBalance(address, nHeight - 1);

            std::cout<<"====="<<address<<":   "<<val<<" + "<<tx.vout[o].nValue;

            val += tx.vout[o].nValue;

            std::cout<<" = "<<val<<std::endl;

            if (cacheBalance.find(address) == cacheBalance.end())
                cacheBalance.insert(pair<std::string, CAmount>(address, val));
            else
                cacheBalance[address] = val;
            if (!WriteDB(address, nHeight, val))
                return false;
        }
    }

    return true;
}
