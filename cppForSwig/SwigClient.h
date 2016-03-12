/***
Set of spoof classes that expose all BDV, wallet and address obj methods to SWIG
and handle the data transmission with the BDM server
***/

#ifndef _SWIGCLIENT_H
#define _SWIGCLIENT_H

#include "BDM_seder.h"
#include "SocketObject.h"
#include "bdmenums.h"
#include "log.h"

inline void StartCppLogging(string fname, int lvl) { STARTLOGGING(fname, (LogLevel)lvl); }
inline void ChangeCppLogLevel(int lvl) { SETLOGLEVEL((LogLevel)lvl); }
inline void DisableCppLogging() { SETLOGLEVEL(LogLvlDisabled); }
inline void EnableCppLogStdOut() { LOGENABLESTDOUT(); }
inline void DisableCppLogStdOut() { LOGDISABLESTDOUT(); }

#include <thread>

class BlockDataViewer;

enum SocketType
{
   SocketBinary,
   SocketHttp,
   SocketFcgi
};

///////////////////////////////////////////////////////////////////////////////
class ScrAddrObj
{
private:
   const string walletID_;
   const BinaryData scrAddr_;
   const shared_ptr<BinarySocket> sock_;

public:
   ScrAddrObj(const BlockDataViewer&, const string&, const BinaryData&);
};

///////////////////////////////////////////////////////////////////////////////
class BtcWallet
{
private:
   const string walletID_;
   const string bdvID_;
   const shared_ptr<BinarySocket> sock_;

public:
   BtcWallet(const BlockDataViewer&, const string&);
   int64_t getFullBalance(void);
   int64_t getUnconfirmedBalance(uint32_t topBlockHeight, bool IGNOREZC);
   int64_t getSpendableBalance(uint32_t topBlockHeight, bool IGNOREZC);
};

///////////////////////////////////////////////////////////////////////////////
class LedgerDelegate
{
private:
   const string delegateID_;
   const string bdvID_;
   const shared_ptr<BinarySocket> sock_;

public:
   LedgerDelegate(BlockDataViewer&, const string&);

   vector<LedgerEntryData> getHistoryPage(uint32_t id);
};

///////////////////////////////////////////////////////////////////////////////
class Blockchain
{
private:
   const shared_ptr<BinarySocket> sock_;
   const string bdvID_;

public:
   Blockchain(const BlockDataViewer&);
   bool hasHeaderWithHash(const BinaryData& hash);
};

///////////////////////////////////////////////////////////////////////////////
class PythonCallback
{   
private:
   struct KillPythonCallback
   {};
   
   bool run_;
   thread thr_;

   const shared_ptr<BinarySocket> sock_;
   const string bdvID_;

public:
   PythonCallback(const BlockDataViewer& bdv);
   virtual ~PythonCallback(void) = 0;

   virtual void run(BDMAction action, void* ptr, int block = 0) = 0;
   virtual void progress(
      BDMPhase phase,
      const vector<string> &walletIdVec,
      float progress, unsigned secondsRem,
      unsigned progressNumeric
      ) = 0;   
   
   void remoteLoop(void);
};

///////////////////////////////////////////////////////////////////////////////
class BlockDataViewer
{
   friend class ScrAddrObj;
   friend class BtcWallet;
   friend class PythonCallback;
   friend class LedgerDelegate;
   friend class Blockchain;

private:
   string bdvID_;
   const shared_ptr<BinarySocket> sock_;

private:
   BlockDataViewer(const shared_ptr<BinarySocket> sock);

public:
   ~BlockDataViewer(void);
   BtcWallet registerWallet(const string& id, 
      const vector<BinaryData>& addrVec,
      bool isNew);

   BtcWallet registerLockbox(const string& id,
      const vector<BinaryData>& addrVec,
      bool isNew);

   const string& getID(void) const { return bdvID_; }

   static BlockDataViewer getNewBDV(
      const string& addr, const string& port, SocketType);
   
   LedgerDelegate getLedgerDelegateForWallets(void);
   LedgerDelegate getLedgerDelegateForLockboxes(void);
   Blockchain blockchain(void);

   void goOnline(void);
};

#endif