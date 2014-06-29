#ifndef _BTCWALLET_H
#define _BTCWALLET_H

#include "BinaryData.h"
#include "BlockObj.h"
#include "ScrAddrObj.h"
#include "StoredBlockObj.h"
#include "ThreadSafeContainer.h"

class BlockDataManager_LevelDB;

typedef map<BinaryData, ScrAddrObj> saMap;
typedef ts_pair_container<saMap> ts_saMap;

////////////////////////////////////////////////////////////////////////////////
class AddressBookEntry
{
public:

   /////
   AddressBookEntry(void) : scrAddr_(BtcUtils::EmptyHash()) { txList_.clear(); }
   explicit AddressBookEntry(BinaryData scraddr) : scrAddr_(scraddr) { txList_.clear(); }
   void addTx(Tx & tx) { txList_.push_back( RegisteredTx(tx) ); }
   BinaryData getScrAddr(void) { return scrAddr_; }

   /////
   vector<RegisteredTx> getTxList(void)
   { 
      sort(txList_.begin(), txList_.end()); 
      return txList_;
   }

   /////
   bool operator<(AddressBookEntry const & abe2) const
   {
      // If one of the entries has no tx (this shouldn't happen), sort by hash
      if( txList_.size()==0 || abe2.txList_.size()==0)
         return scrAddr_ < abe2.scrAddr_;

      return (txList_[0] < abe2.txList_[0]);
   }

private:
   BinaryData scrAddr_;
   vector<RegisteredTx> txList_;
};

////////////////////////////////////////////////////////////////////////////////
//
// BtcWallet
//
////////////////////////////////////////////////////////////////////////////////
class BtcWallet
{
public:
   BtcWallet(BlockDataManager_LevelDB* bdm)
      : bdmPtr_(bdm),
      lastScanned_(0),
      ignoreLastScanned_(true),
      isInitialized_(false),
      isRegistered_(false)
   {}
   ~BtcWallet(void);

   /////////////////////////////////////////////////////////////////////////////
   // addScrAddr when blockchain rescan req'd, addNewScrAddr for just-created
   void addNewScrAddress(BinaryData addr);
   void addScrAddress(ScrAddrObj const & newAddr);
   void addScrAddress(BinaryData    addr, 
                   uint32_t      firstTimestamp = 0,
                   uint32_t      firstBlockNum  = 0,
                   uint32_t      lastTimestamp  = 0,
                   uint32_t      lastBlockNum   = 0);

   // SWIG has some serious problems with typemaps and variable arg lists
   // Here I just create some extra functions that sidestep all the problems
   // but it would be nice to figure out "typemap typecheck" in SWIG...
   void addScrAddress_ScrAddrObj_(ScrAddrObj const & newAddr);

   // Adds a new address that is assumed to be imported, and thus will
   // require a blockchain scan
   void addScrAddress_1_(BinaryData addr);

   // Adds a new address that we claim has never been seen until thos moment,
   // and thus there's no point in doing a blockchain rescan.
   void addNewScrAddress_1_(BinaryData addr) {addNewScrAddress(addr);}

   // Blockchain rescan will depend on the firstBlockNum input
   void addScrAddress_3_(BinaryData    addr, 
                      uint32_t      firstTimestamp,
                      uint32_t      firstBlockNum);

   // Blockchain rescan will depend on the firstBlockNum input
   void addScrAddress_5_(BinaryData    addr, 
                      uint32_t      firstTimestamp,
                      uint32_t      firstBlockNum,
                      uint32_t      lastTimestamp,
                      uint32_t      lastBlockNum);

   bool hasScrAddress(BinaryData const & scrAddr) const;


   // Scan a Tx for our TxIns/TxOuts.  Override default blk vals if you think
   // you will save time by not checking addresses that are much newer than
   // the block

   void scanNonStdTx(uint32_t    blknum, 
                     uint32_t    txidx, 
                     Tx &        txref,
                     uint32_t    txoutidx,
                     ScrAddrObj& addr);

   // BlkNum is necessary for "unconfirmed" list, since it is dependent
   // on number of confirmations.  But for "spendable" TxOut list, it is
   // only a convenience, if you want to be able to calculate numConf from
   // the Utxos in the list.  If you don't care (i.e. you only want to 
   // know what TxOuts are available to spend, you can pass in 0 for currBlk
   uint64_t getFullBalance(void) const;
   uint64_t getSpendableBalance(uint32_t currBlk=0, 
                                bool ignoreAllZeroConf=false) const;
   uint64_t getUnconfirmedBalance(uint32_t currBlk,
                                  bool includeAllZeroConf=false) const;

   vector<UnspentTxOut> getSpendableTxOutList(
      uint32_t currBlk=0,
      bool ignoreAllZeroConf=false
   ) const;

   vector<LedgerEntry>
      getTxLedger(BinaryData const &scrAddr) const; 
   vector<LedgerEntry>
      getTxLedger() const; 

   void pprintLedger() const;
   void pprintAlot(InterfaceToLDB *db, uint32_t topBlk=0, bool withAddr=false) const;
   void pprintAlittle(std::ostream &os) const;

   void clearBlkData(void);
   
   vector<AddressBookEntry> createAddressBook(void) const;

   map<BinaryData, TxIOPair> getHistoryForScrAddr(
      BinaryDataRef uniqKey, 
      uint32_t startBlock,
      uint32_t endBlock,
      bool withMultisig=false) const;

   void reset(void);

   //new all purpose wallet scanning call
   void scanWallet(uint32_t startBlock=UINT32_MAX, 
                   uint32_t endBlock=UINT32_MAX,
                   bool forceRescan=false);
   
   //wallet side reorg processing
   void updateAfterReorg(uint32_t lastValidBlockHeight);   
   void scanWalletZeroConf(uint32_t height);
   
   const map<BinaryData, ScrAddrObj> getScrAddrMap(void) const
   { return scrAddrMap_; }

   uint32_t getNumScrAddr(void) const { return scrAddrMap_.size(); }
   void fetchDBScrAddrData(uint32_t startBlock, uint32_t endBlock);
   void fetchDBScrAddrData(ScrAddrObj & scrAddr, 
                                     uint32_t startBlock, uint32_t endBlock);

   void setRegistered(bool isTrue = true) { isRegistered_ = isTrue; }
   void purgeZeroConfTxIO(const set<BinaryData>& invalidatedTxIO);

   const ScrAddrObj* getScrAddrObjByKey(BinaryData key) const
   {
      auto saIter = scrAddrMap_.find(key);
      if (saIter != scrAddrMap_.end())
         return &saIter->second;

      return nullptr;
   }

   void updateLedgers(uint32_t startBlock, uint32_t endBlock);
   void purgeLedgerFromHeight(uint32_t height);

   LedgerEntry getLedgerEntryForTx(const BinaryData& txHash) const;

private:
   const vector<LedgerEntry>& getEmptyLedger(void) 
   { EmptyLedger_.clear(); return EmptyLedger_; }
   void sortLedger();

private:
   BlockDataManager_LevelDB*const      bdmPtr_;
   map<BinaryData, ScrAddrObj>         scrAddrMap_;
   
   bool                                ignoreLastScanned_;
   vector<LedgerEntry>                 ledgerAllAddr_;
   static vector<LedgerEntry>          EmptyLedger_; // just a null-reference object

   bool                                isInitialized_;
   bool                                isRegistered_;

   uint32_t                            lastScanned_;

   BtcWallet(const BtcWallet&); // no copies
};

#endif
// kate: indent-width 3; replace-tabs on;
