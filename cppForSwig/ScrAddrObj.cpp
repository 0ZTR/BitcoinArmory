#include "ScrAddrObj.h"

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// ScrAddrObj Methods
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
ScrAddrObj::ScrAddrObj(LMDBBlockDatabase *db, Blockchain *bc,
                       HashString    addr, 
                       uint32_t      firstBlockNum,
                       uint32_t      firstTimestamp,
                       uint32_t      lastBlockNum,
                       uint32_t      lastTimestamp) :
      db_(db),
      bc_(bc),
      scrAddr_(addr), 
      firstBlockNum_(firstBlockNum), 
      firstTimestamp_(firstTimestamp),
      lastBlockNum_(lastBlockNum), 
      lastTimestamp_(lastTimestamp),
      txnPerPage_(100)
{ 
   relevantTxIO_.clear();
} 



////////////////////////////////////////////////////////////////////////////////
uint64_t ScrAddrObj::getSpendableBalance(
   uint32_t currBlk, bool ignoreAllZC
) const
{
   //ignoreing the currBlk for now, until the partial history loading is solid
   uint64_t balance = getFullBalance();

   for (auto txio : relevantTxIO_)
   {
      if (!txio.second.isSpendable(db_, currBlk, ignoreAllZC) && 
          !txio.second.hasTxIn())
         balance -= txio.second.getValue();
   }

   return balance;
}


////////////////////////////////////////////////////////////////////////////////
uint64_t ScrAddrObj::getUnconfirmedBalance(
   uint32_t currBlk, bool inclAllZC
) const
{
   /***may need some help with the paging system in place***/

   uint64_t balance = 0;
   for (auto txio : relevantTxIO_)
   {
      if(txio.second.isMineButUnconfirmed(db_, currBlk, inclAllZC))
         balance += txio.second.getValue();
   }
   return balance;
}

////////////////////////////////////////////////////////////////////////////////
uint64_t ScrAddrObj::getFullBalance() const
{
   StoredScriptHistory ssh;
   db_->getStoredScriptHistorySummary(ssh, scrAddr_);
   uint64_t balance = ssh.getScriptBalance(false);

   for (auto txio : relevantTxIO_)
   {
      if (txio.second.hasTxInZC())
         balance -= txio.second.getValue();
      else if (txio.second.hasTxOutZC())
         balance += txio.second.getValue();
   }
   return balance;
}

////////////////////////////////////////////////////////////////////////////////
vector<UnspentTxOut> ScrAddrObj::getSpendableTxOutList(
   uint32_t blkNum,
   bool ignoreAllZC
) const
{
   vector<UnspentTxOut> utxoList(0);
   for (auto txio : relevantTxIO_)
   {
      if(txio.second.isSpendable(db_, blkNum, ignoreAllZC))
      {
         TxOut txout = txio.second.getTxOutCopy(db_);
         utxoList.push_back( UnspentTxOut(db_, txout, blkNum) );
      }
   }
   return utxoList;
}

////////////////////////////////////////////////////////////////////////////////
vector<UnspentTxOut> ScrAddrObj::getFullTxOutList(uint32_t blkNum) const
{
   vector<UnspentTxOut> utxoList(0);
   for (auto txio : relevantTxIO_)
   {
      if(txio.second.isUnspent(db_))
      {
         TxOut txout = txio.second.getTxOutCopy(db_);
         utxoList.push_back( UnspentTxOut(db_, txout, blkNum) );
      }
   }
   return utxoList;
}
   
////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::addTxIO(TxIOPair& txio, bool isZeroConf)
{ 
   relevantTxIO_[txio.getDBKeyOfOutput()] = txio;
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::pprintLedger() const 
{ 
   cout << "Address Ledger: " << getScrAddr().toHexStr() << endl;
   for(const auto ledger : *ledger_)
      ledger.second.pprintOneLine();
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::clearBlkData(void)
{
   relevantTxIO_.clear();
   hist_.reset();
   ledger_ = nullptr;
   totalTxioCount_ = 0;
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::updateTxIOMap(map<BinaryData, TxIOPair>& txio_map)
{
   for (auto txio : txio_map)
      relevantTxIO_[txio.first] = txio.second;
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::scanZC(const map<HashString, TxIOPair>& zcTxIOMap)
{
   for (auto txioPair : zcTxIOMap)
      relevantTxIO_[txioPair.first] = txioPair.second;
   
   updateLedgers(*ledger_, zcTxIOMap);
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::purgeZC(const vector<BinaryData>& invalidatedTxOutKeys)
{
   for (auto zc : invalidatedTxOutKeys)
   {
      auto txioIter = relevantTxIO_.find(zc);

      if (ITER_IN_MAP(txioIter, relevantTxIO_))
      {
         TxIOPair& txio = txioIter->second;

         if (txio.hasTxInZC())
         {
            //ZC consumes UTxO, reset the TxIn to mark the TxOut as unspent
            LedgerEntry &le = (*ledger_)[zc.getSliceRef(0, 6)];
            le.setValue(le.getValue() + txio.getValue());

            //since the txio has a ZC txin, there is a scrAddr ledger entry for that key
            ledger_->erase(txio.getTxRefOfInput().getDBKey());
            
            txio.setTxIn(BinaryData(0));
            txio.setTxHashOfInput(BinaryData(0));
         }

         if (txio.hasTxOutZC())
         {
            //purged ZC chain, remove the TxIO
            relevantTxIO_.erase(txioIter);
            ledger_->erase(zc.getSliceRef(0, 6));
         }
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::updateAfterReorg(uint32_t lastValidBlockHeight)
{
   auto txioIter = relevantTxIO_.begin();

   uint32_t height;
   while (txioIter != relevantTxIO_.end())
   {
      //txio pairs are saved by TxOut DBkey, if the key points to a block 
      //higher than the reorg point, delete the txio
      height = DBUtils::hgtxToHeight(txioIter->first.getSliceCopy(0, 4));

      if (height >= 0xFF000000)
      {
         //ZC chain, already dealt with by the call to purgeZC from 
         //readBlkFileUpdate
         continue;
      }
      else if (height <= lastValidBlockHeight)
      {
         TxIOPair& txio = txioIter->second;
         if (txio.hasTxIn())
         {
            //if the txio is spent, check the block of the txin
            height = DBUtils::hgtxToHeight(
               txio.getDBKeyOfInput().getSliceCopy(0, 4));

            if (height > lastValidBlockHeight && height < 0xFF000000)
            {
               //clear the TxIn by setting it to an empty BinaryData
               txio.setTxIn(BinaryData(0));
               txio.setTxHashOfInput(BinaryData(0));
            }
         }

         ++txioIter;
      }
      else
         relevantTxIO_.erase(txioIter++);
   }

   //clean up ledgers
   BinaryData cutOffHghtX = DBUtils::heightAndDupToHgtx(lastValidBlockHeight + 1, 0);
   uint16_t zero = 0;
   cutOffHghtX.append((uint8_t*)&zero, 2);

   auto leRange = ledger_->equal_range(cutOffHghtX);
   ledger_->erase(leRange.first, ledger_->end());
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::updateLedgers(map<BinaryData, LedgerEntry>& myLedger,
                               const map<BinaryData, TxIOPair>& newTxio,
                               uint32_t startBlock) const
{
   /***
   Nothing too complicated here. A map of new TxIOPair ordered by Tx DBkey
   are parsed to create the respective scrAddr ledger entries. 
   
   A TxIn is a spend, thus the ledger entry value will be negative. 
   A TxOut marks received funds, and will have a positive value. 

   TxIOPairs carry the coinbase distiction at SSH level.
   The concept of change does not apply at scrAddr level.

   Height and index are derived from DBkeys.
   
   The TxHash is queried from the db since it isn't carried by the TxIOPair.
   The timestamp is the block timestamp. Blockchain objects do not allow to
   query blocks by height AND dup, and will only return block headers for the 
   main chain at any given height, however that is irrelevant here, as only 
   main branch transactions make it into SSH objects.
   ***/

   BinaryData opKey;
   BinaryData inKey;
   BinaryData txKey;
   BlockHeader*  bhptr;
   uint32_t txtime;

   uint32_t opHeight;
   uint32_t inHeight;
   uint32_t opIdx;
   uint32_t inIdx;

   for (auto txioPair : newTxio)
   {
      const TxIOPair& txio = txioPair.second;
      
      opKey = txio.getDBKeyOfOutput();
      txKey = txio.getDBKeyOfOutput().getSliceCopy(0, 6);
      auto leIter = myLedger.find(txKey);

      if (ITER_NOT_IN_MAP(leIter, myLedger))
      {
         if ((uint16_t)*opKey.getSliceRef(0, 2).getPtr() == 0xFF)
         {
            opHeight = UINT32_MAX;
            opIdx = READ_UINT32_BE(opKey.getSliceRef(2, 4));
         }
         else
         {
            opHeight = DBUtils::hgtxToHeight(opKey.getSliceRef(0, 4));
            opIdx = READ_UINT16_BE(opKey.getSliceRef(4, 2));
         }

         if (opHeight >= startBlock)
         {
            txtime = txio.getTxTime();
            if (txtime == 0)
            {
               bhptr = &bc_->getHeaderByHeight(opHeight);
               txtime = bhptr->getTimestamp();
            }

            LedgerEntry le(scrAddr_,
               txio.getValue(),
               opHeight,
               txio.getTxHashOfOutput(db_),
               opIdx,
               txtime,
               txio.isFromCoinbase());

            myLedger[txKey] = le;
         }
      }
      else if (!txio.hasTxIn())
      {
         leIter->second.setValue(leIter->second.getValue() +
            (int64_t)txio.getValue());
      }

      if (txio.hasTxIn())
      {
         inKey = txio.getDBKeyOfInput().getSliceCopy(0, 8);
         txKey = txio.getDBKeyOfInput().getSliceCopy(0, 6);
         auto leIter = myLedger.find(txKey);

         if (ITER_NOT_IN_MAP(leIter, myLedger))
         {
            if ((uint16_t)*inKey.getSliceRef(0, 2).getPtr() == 0xFF)
            {
               inHeight = UINT32_MAX;
               inIdx = READ_UINT32_BE(inKey.getSliceRef(2, 4));
            }
            else
            {
               inHeight = DBUtils::hgtxToHeight(inKey.getSliceRef(0, 4));
               inIdx = READ_UINT16_BE(inKey.getSliceRef(4, 2));
            }

            if (inHeight >= startBlock)
            {
               txtime = txio.getTxTime();
               if (txtime == 0)
               {
                  bhptr = &bc_->getHeaderByHeight(inHeight);
                  txtime = bhptr->getTimestamp();
               }

               LedgerEntry le(scrAddr_,
                  (int64_t)txio.getValue() * -1,
                  inHeight,
                  txio.getTxHashOfInput(db_),
                  inIdx,
                  txtime);

               myLedger[txKey] = le;
            }
         }
         else 
            leIter->second.setValue(leIter->second.getValue() -
                                    (int64_t)txio.getValue());
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
uint64_t ScrAddrObj::getTxioCountFromSSH(void) const
{
   StoredScriptHistory ssh;
   db_->getStoredScriptHistorySummary(ssh, scrAddr_);

   return ssh.totalTxioCount_;
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::fetchDBScrAddrData(uint32_t startBlock,
                                    uint32_t endBlock)
{
   if (endBlock < lastSeenBlock_ && totalTxioCount_ == 0)
      return;

   map<BinaryData, TxIOPair> hist = getHistoryForScrAddr(startBlock, endBlock);
   
   updateTxIOMap(hist);
   updateLedgers(*ledger_, hist);
}

///////////////////////////////////////////////////////////////////////////////
map<BinaryData, TxIOPair> ScrAddrObj::getHistoryForScrAddr(
   uint32_t startBlock, uint32_t endBlock,
   bool withMultisig) const
{
   StoredScriptHistory ssh;
   db_->getStoredScriptHistory(ssh, scrAddr_, startBlock, endBlock);

   totalTxioCount_ = ssh.totalTxioCount_;
   lastSeenBlock_ = endBlock;

   if (scrAddr_[0] == SCRIPT_PREFIX_MULTISIG)
      withMultisig = true;

   map<BinaryData, TxIOPair> outMap;
   if (!ssh.isInitialized())
      return outMap;

   for (auto &subSSHEntry : ssh.subHistMap_)
   {
      StoredSubHistory & subssh = subSSHEntry.second;

      for (auto &txiop : subssh.txioMap_)
      {
         const TxIOPair & txio = txiop.second;
         if (withMultisig || !txio.isMultisig())
            outMap[txiop.first] = txio;
      }
   }

   return outMap;
}

////////////////////////////////////////////////////////////////////////////////
vector<LedgerEntry> ScrAddrObj::getHistoryPageById(uint32_t id)
{
   if (id < 0)
   {
      return vector<LedgerEntry>();
   }

   return getTxLedgerAsVector(hist_.getPageLedgerMap(this, id));
}

////////////////////////////////////////////////////////////////////////////////
void ScrAddrObj::mapHistory()
{
   //create history map
   hist_.mapScrAddrHistory(db_, scrAddr_, txnPerPage_); 

   //grab first page and point ScrAddrObj's ledger at it
   ledger_ = &hist_.getPageLedgerMap(this, 0, &relevantTxIO_);
}

////////////////////////////////////////////////////////////////////////////////
ScrAddrObj& ScrAddrObj::operator= (const ScrAddrObj& rhs)
{
   if (&rhs == this)
      return *this;

   db_ = rhs.db_;
   bc_ = rhs.bc_;

   scrAddr_ = rhs.scrAddr_;
   firstBlockNum_ = rhs.firstBlockNum_;
   firstTimestamp_ = rhs.firstTimestamp_;
   lastBlockNum_ = rhs.lastBlockNum_;
   lastTimestamp_ = rhs.lastTimestamp_;

   hasMultisigEntries_ = rhs.hasMultisigEntries_;

   relevantTxIO_ = rhs.relevantTxIO_;

   totalTxioCount_ = rhs.totalTxioCount_;
   lastSeenBlock_ = rhs.lastSeenBlock_;

   //prebuild history indexes for quick fetch from SSH
   hist_ = rhs.hist_;
   txnPerPage_ = rhs.txnPerPage_;
   
   ledger_ = nullptr;
   if (hist_.getPageCount() != 0)
      ledger_ = &hist_.getPageLedgerMap(this, 0);
}

////////////////////////////////////////////////////////////////////////////////
vector<LedgerEntry> ScrAddrObj::getTxLedgerAsVector(
   map<BinaryData, LedgerEntry>& leMap) const
{
   vector<LedgerEntry>le;

   for (auto& lePair : leMap)
      le.push_back(lePair.second);

   //sort(le.begin(), le.end(), LedgerEntry::greaterThan);
   return le;
}


////////////////////////////////////////////////////////////////////////////////
//
// HistoryPages
//
////////////////////////////////////////////////////////////////////////////////
void HistoryPages::addPage(uint32_t count, uint32_t bottom, uint32_t top)
{
   Page newPage(count, bottom, top);
   pages_.push_back(newPage);
}

////////////////////////////////////////////////////////////////////////////////
map<BinaryData, LedgerEntry>& HistoryPages::getPageLedgerMap(
   const ScrAddrObj* sa, uint32_t pageId, 
   map<BinaryData, TxIOPair>* txioMap)
{
   Page& page = pages_[pageId];

   if (page.pageLedgers_.size() != 0)
   {
      //already loaded this page
      return page.pageLedgers_;
   }

   page.pageLedgers_.clear();

   //load page's block range from SSH
   if (txioMap != nullptr)
   {
      *txioMap = sa->getHistoryForScrAddr(page.blockStart_, page.blockEnd_);
      sa->updateLedgers(page.pageLedgers_, *txioMap, page.blockStart_);
   }
   else
   {
      map<BinaryData, TxIOPair> txio = sa->getHistoryForScrAddr(page.blockStart_, page.blockEnd_);
      sa->updateLedgers(page.pageLedgers_, txio, page.blockStart_);
   }
   //build ledgers

   return page.pageLedgers_;
}

////////////////////////////////////////////////////////////////////////////////
void HistoryPages::mapScrAddrHistory(LMDBBlockDatabase *db,
                                     const BinaryData& scrAddr, 
                                     uint32_t txnPerPage)
{
   //grab the SSH summary for the scrAddr. This is a map, referencing the amount
   //of txio per block for the given address.
   SSHsummary_ = db->getSSHSummary(scrAddr, UINT32_MAX);

   reset();

   auto histIter = SSHsummary_.crbegin();
   uint32_t threshold = 0;
   uint32_t top;

   while (histIter != SSHsummary_.crend())
   {
      if (threshold == 0)
         top = histIter->first;

      threshold += histIter->second;
      if (threshold > txnPerPage)
      {
         addPage(threshold, histIter->first, top);

         threshold = 0;
      }

      ++histIter;
   }

   if (threshold != 0)
      addPage(threshold, 0, top);

   if (pages_.size() == 0)
      addPage(0, 0, UINT32_MAX);

   sortPages();
}

////////////////////////////////////////////////////////////////////////////////
uint32_t HistoryPages::getPageBottom(uint32_t id) const
{ 
   if (id < pages_.size()) 
      return pages_[id].blockStart_; 

   return 0;
}

