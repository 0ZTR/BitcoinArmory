from os import path
import base64
import json
import ast
from armoryengine.ArmoryUtils import *
from armoryengine.Transaction import *
from armorycolors import htmlColor

MULTISIG_VERSION = 0

################################################################################
#
# Multi-signature transactions are going to require a ton of different 
# primitives to be both useful and safe for escrow.  All primitives should
# have an ASCII-armored-esque form for transmission through email or text
# file, as well as binary form for when file transfer is guaranteed
#
# Until Armory implements BIP 32, these utilities are more suited to
# low-volume use cases, such as one-time escrow, or long-term savings
# using multi-device authentication.  Multi-signature *wallets* which
# behave like regular wallets but spit out P2SH addresses and usable 
# in every day situations -- those will have to wait for Armory's new
# wallet format.
#
# Concepts:
#     "Lockbox":  A "lock box" for putting coins that will be protected
#                  with multiple signatures.  The lockbox contains both
#                  the script info as well as meta-data, like participants'
#                  names and emails associated with each public key.
#
#
# 
#     
#                  
################################################################################
"""
Use-Case 1 -- Protecting coins with 2-of-3 computers (2 offline, 1 online):

   Create or access existing wallet on each of three computers. 

   Online computer will create the lockbox - needs one public key from its
   own wallet, and one from each offline wallet.  Can have both WO wallets
   on the online computer, and pull keys directly from those.

   User creates an lockbox with all three keys, labeling them appropriately
   This lockbox will be added to the global list.

   User will fund the lockbox from an existing offline wallet with lots of
   money.  He does not need to use the special funding procedure, which is
   only needed if there's multiple people involved with less than full trust.
   
   Creates the transaction as usual, but uses the "lockbox" button for the
   recipient instead of normal address.  The address line will show the 
   lockbox ID and short description.  

   Will save the lockbox and the offline transaction to the USB drive

"""

LOCKBOXIDSIZE = 8
PROMIDSIZE = 4
LBPREFIX, LBSUFFIX = 'Lockbox[Bare:', ']'
LBP2SHPREFIX = 'Lockbox['

################################################################################
def calcLockboxID(script=None, scraddr=None):
   # ScrAddr is "Script/Address" and for multisig it is 0xfe followed by
   # M and N, then the SORTED hash160 values of the public keys
   # Part of the reason for using "ScrAddrs" is to bundle together
   # different scripts that have the same effective signing authority.
   # Different sortings of the same public key list have same signing
   # authority and therefore should have the same ScrAddr

   if script is not None:
      scrType = getTxOutScriptType(script)
      if not scrType==CPP_TXOUT_MULTISIG:
         LOGERROR('Not a multisig script!')
         return None
      scraddr = script_to_scrAddr(script)

   if not scraddr.startswith(SCRADDR_MULTISIG_BYTE):
      LOGERROR('ScrAddr is not a multisig script!')
      return None

   hashedData = hash160(MAGIC_BYTES + scraddr)
   #M,N = getMultisigScriptInfo(script)[:2]
   #return '%d%d%s' % (M, N, binary_to_base58(hashedData)[:6])

   # Using letters 1:9 because the first letter has a minimal range of 
   # values for 32-bytes converted to base58
   return binary_to_base58(hashedData)[1:9]


################################################################################
def createLockboxEntryStr(lbID, isBareMultiSig=False):
   return '%s%s%s' % (LBPREFIX if isBareMultiSig else LBP2SHPREFIX,
                       lbID, LBSUFFIX)

################################################################################
def readLockboxEntryStr(addrtext):
   result = None
   if isLockbox(addrtext) or isP2SHLockbox(addrtext):
      len(LBPREFIX if isLockbox(addrtext) else LBP2SHPREFIX)
      idStr = addrtext[len(LBPREFIX if isLockbox(addrtext) else LBP2SHPREFIX):
                       addrtext.find(LBSUFFIX)]
      if len(idStr)==LOCKBOXIDSIZE:
         result = idStr
   return result

################################################################################
def isP2SHLockbox(addrtext):
   return addrtext.startswith(LBP2SHPREFIX)

################################################################################
def isLockbox(addrtext):
   return addrtext.startswith(LBPREFIX)

################################################################################
################################################################################
class MultiSigLockbox(object):

   #############################################################################
   def __init__(self, script=None, name=None, descr=None, \
                                          commList=None, createDate=None):
      self.version   = 0
      self.binScript = script
      self.shortName = name
      self.longDescr = toUnicode(descr)
      self.commentList = commList
      self.createDate = long(RightNow()) if createDate is None else createDate
      self.magicBytes = MAGIC_BYTES

      if script is not None:
         self.setParams(script, name, descr, commList)


   #############################################################################
   def setParams(self, script, name=None, descr=None, commList=None, \
                                 createDate=None, version=MULTISIG_VERSION):
      
      # Set params will only overwrite with non-None data
      self.binScript = script
      
      if name is not None:
         self.shortName = name

      if descr is not None:
         self.longDescr = toUnicode(descr)

      if commList is not None:
         self.commentList = commList[:]

      if createDate is not None:
         self.createDate = createDate

      self.version = version
      self.magicBytes = MAGIC_BYTES

      scrType = getTxOutScriptType(script)
      if not scrType==CPP_TXOUT_MULTISIG:
         LOGERROR('Attempted to create lockbox from non-multi-sig script')
         self.binScript = None
         return


      # Computed some derived members
      self.scrAddr      = script_to_scrAddr(script)
      self.p2shScrAddr  = script_to_scrAddr(script_to_p2sh_script(script))
      self.uniqueIDB58  = calcLockboxID(script)
      self.M, self.N, self.a160List, self.pubKeys = getMultisigScriptInfo(script)
      self.opStrList = convertScriptToOpStrings(script)

      
      
   #############################################################################
   def serialize(self):

      bp = BinaryPacker()
      bp.put(UINT32,       self.version)
      bp.put(BINARY_CHUNK, MAGIC_BYTES)
      bp.put(UINT64,       self.createDate)
      bp.put(VAR_STR,      self.binScript)
      bp.put(VAR_STR,      toBytes(self.shortName))
      bp.put(VAR_STR,      toBytes(self.longDescr))
      bp.put(UINT32,       len(self.commentList))
      for comm in self.commentList:
         bp.put(VAR_STR,   toBytes(comm))

      return bp.getBinaryString()



   #############################################################################
   def unserialize(self, rawData, expectID=None):

      bu = BinaryUnpacker(rawData)
      boxVersion = bu.get(UINT32)
      boxMagic   = bu.get(BINARY_CHUNK, 4)
      created    = bu.get(UINT64)
      boxScript  = bu.get(VAR_STR)
      boxName    = toUnicode(bu.get(VAR_STR))
      boxDescr   = toUnicode(bu.get(VAR_STR))
      nComment   = bu.get(UINT32)

      boxComms = ['']*nComment
      for i in range(nComment):
         boxComms[i] = toUnicode(bu.get(VAR_STR))

      # Issue a warning if the versions don't match
      if not boxVersion == MULTISIG_VERSION:
         LOGWARN('Unserialing lockbox of different version')
         LOGWARN('   Lockbox Version: %d' % boxVersion)
         LOGWARN('   Armory  Version: %d' % MULTISIG_VERSION)

      # Check the magic bytes of the lockbox match
      if not boxMagic == MAGIC_BYTES:
         LOGERROR('Wrong network!')
         LOGERROR('    Lockbox Magic: ' + binary_to_hex(boxMagic))
         LOGERROR('    Armory  Magic: ' + binary_to_hex(MAGIC_BYTES))
         raise NetworkIDError('Network magic bytes mismatch')

      
      # Lockbox ID is written in the first line, it should match the script
      # If not maybe a version mistmatch, serialization error, or bug
      if expectID and not calcLockboxID(boxScript) == expectID:
         LOGERROR('ID on lockbox block does not match script')
         raise UnserializeError('ID on lockbox does not match!')

      # No need to read magic bytes -- already checked & bailed if incorrect
      self.setParams(boxScript, boxName, boxDescr, boxComms, created)

      return self


   #############################################################################
   def serializeAscii(self, wid=80, newline='\n'):
      headStr = 'LOCKBOX-%s' % self.uniqueIDB58
      return makeAsciiBlock(self.serialize(), headStr, wid, newline)


   #############################################################################
   def unserializeAscii(self, boxBlock):
      headStr, rawData = readAsciiBlock(boxBlock, 'LOCKBOX')
      if rawData is None:
         LOGERROR('Expected header str "LOCKBOX", got "%s"' % headStr)
         raise UnserializeError('Expected LOCKBOX block, got something else')

      # We should have "LOCKBOX-BOXID" in the headstr
      boxID = headStr.split('-')[-1]
      return self.unserialize(rawData, boxID)

   #############################################################################
   def pprint(self):
      print 'Multi-signature %d-of-%d lockbox:' % (self.M, self.N)
      print '   Unique ID:  ', self.uniqueIDB58
      print '   Created:    ', unixTimeToFormatStr(self.createDate)
      print '   Box Name:   ', self.shortName
      print '   P2SHAddr:   ', scrAddr_to_addrStr(self.p2shScrAddr)
      print '   Box Desc:   '
      print '     ', self.longDescr[:70]
      print '   Key List:   '
      print '   Script Ops: '
      for opStr in self.opStrList:
         print '       ', opStr
      print''
      print '   Key Info:   '
      for i in range(len(self.pubKeys)):
         print '            Key %d' % i
         print '           ', binary_to_hex(self.pubKeys[i])[:40] + '...'
         print '           ', hash160_to_addrStr(self.a160List[i])
         print '           ', self.commentList[i]
         print ''
      


   #############################################################################
   def pprintOneLine(self):
      print 'LockBox %s:  %s-of-%s, created: %s;  "%s"' % (self.uniqueIDB58, 
         self.M, self.N, unixTimeToFormatStr(self.createDate), self.shortName)

   #############################################################################
   def getDisplayRichText(self, tr=None, dateFmt=None):

      if dateFmt is None:
         dateFmt = DEFAULT_DATE_FORMAT

      if tr is None:
         tr = lambda x: unicode(x)

      EMPTYLINE = u''

      shortName = toUnicode(self.shortName)
      if len(shortName.strip())==0:
         shortName = u'<No Lockbox Name'

      longDescr = toUnicode(self.longDescr)
      if len(longDescr.strip())==0:
         longDescr = '--- No Extended Info ---'
      longDescr = longDescr.replace('\n','<br>')
      longDescr = longDescr.replace(' ','&nbsp;')
      formattedDate = unixTimeToFormatStr(self.createDate, dateFmt)
      
      lines = []
      lines.append(tr("""<font color="%s"><font size=6><center>Lockbox Information for 
         <b>%s</font></b>""") % (htmlColor("TextBlue"), self.uniqueIDB58))
      lines.append(tr('<b>Multisig:</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;%d-of-%d') % (self.M, self.N))
      lines.append(tr('<b>Lockbox ID:</b>&nbsp;&nbsp;&nbsp;&nbsp;%s') % self.uniqueIDB58)
      lines.append(tr('<b>P2SH Address:</b>&nbsp;&nbsp;%s') % binScript_to_p2shAddrStr(self.binScript))
      lines.append(tr('<b>Lockbox Name:</b>&nbsp;&nbsp;%s') % self.shortName)
      lines.append(tr('<b>Created:</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;%s') % formattedDate) 
      lines.append(tr('<b>Extended Info:</b><hr><blockquote>%s</blockquote><hr>') % longDescr)
      lines.append(tr('<b>Stored Key Details</b>'))
      for i in range(len(self.pubKeys)):
         comm = self.commentList[i]
         addr = hash160_to_addrStr(self.a160List[i])
         pubk = binary_to_hex(self.pubKeys[i])[:40] + '...'

         if len(comm.strip())==0:
            comm = '<No Info>'

         lines.append(tr('&nbsp;&nbsp;<b>Key #%d</b>') % (i+1))
         lines.append(tr('&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>Name/ID:</b>&nbsp;%s') % comm)
         lines.append(tr('&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>Address:</b>&nbsp;%s') % addr)
         lines.append(tr('&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>PubKey:</b>&nbsp;&nbsp;%s') % pubk)
         lines.append(EMPTYLINE)
      lines.append(tr('</font>'))
      return '<br>'.join(lines)


   ################################################################################
   def createDecoratedTxOut(self, value=0, asP2SH=False):
      if not asP2SH:
         dtxoScript = self.binScript
         p2shScript = None
      else:
         dtxoScript = script_to_p2sh_script(self.binScript)
         p2shScript = self.binScript

      return DecoratedTxOut(dtxoScript, value, p2shScript)
      


   ################################################################################
   def makeFundingTxFromPromNotes(self, promList):
      ustxiAccum = []
      
      totalPay = sum([prom.dtxoTarget.value for prom in promList])
      totalFee = sum([prom.feeAmt for prom in promList])

      # DTXO list always has at least the lockbox itself
      dtxoAccum  = [self.createDecoratedTxOut(value=totalPay, asP2SH=False)]

      # Errors with the change values should've been caught in prom::setParams
      totalInputs = 0
      totalChange = 0
      for prom in promList:
         for ustxi in prom.ustxInputs:
            ustxiAccum.append(ustxi)
            totalInputs += ustxi.value

         # Add any change outputs
         if prom.dtxoChange.value > 0:
            dtxoAccum.append(prom.dtxoChange)
            totalChange += prom.dtxoChange.value
      
      if not totalPay + totalFee == totalInputs - totalChange:
         raise ValueError('Promissory note values do not add up correctly')

      return UnsignedTransaction().createFromUnsignedTxIO(ustxiAccum, dtxoAccum)


   ################################################################################
   def makeSpendingTx(self, rawFundTxIdxPairs, dtxoList, feeAmt):

      ustxiAccum = []
      
      # Errors with the change values should've been caught in setParams
      totalInputs = 0
      anyP2SH = False
      for rawTx,txoIdx in rawFundTxIdxPairs:
         fundTx    = PyTx().unserialize(rawTx)
         txout     = fundTx.outputs[txoIdx]
         txoScript = txout.getScript()
         txoValue  = txout.getValue()

         if not calcLockboxID(txoScript)==self.uniqueIDB58:
            raise InvalidScriptError('Given OutPoint is not for this lockbox')

         # If the funding tx is P2SH, make sure it matches the lockbox
         # then include the subscript in the USTXI
         p2shSubscript = None
         if getTxOutScriptType(txoScript) == CPP_TXOUT_P2SH:
            # setParams guarantees self.binScript is bare multi-sig script
            txP2SHScrAddr = script_to_scrAddr(txoScript)
            lbP2SHScrAddr = script_to_p2sh_script(self.binScript)
            if not lbP2SHScrAddr == txP2SHScrAddr:
               LOGERROR('Given utxo script hash does not match this lockbox')
               raise InvalidScriptError('P2SH input does not match lockbox')
            p2shSubscript = self.binScript
            anyP2SH = True
            

         ustxiAccum.append(UnsignedTxInput(rawTx, txoIdx, p2shSubscript))
         totalInputs += txoValue


      # Copy the dtxoList since we're probably adding a change output
      dtxoAccum = dtxoList[:]

      totalOutputs = sum([dtxo.value for dtxo in dtxoAccum])
      changeAmt = totalInputs - (totalOutputs + feeAmt)
      if changeAmt < 0:
         raise ValueError('More outputs than inputs!')
      elif changeAmt > 0:
         # If adding change output, make it P2SH if any inputs were P2SH
         if not anyP2SH:
            txoScript = self.binScript
            p2shScript = None
         else:
            txoScript = script_to_p2sh_script(self.binScript)
            p2shScript = self.binScript
         dtxoAccum.append( DecoratedTxOut(txoScript, changeAmt, p2shScript))

      return UnsignedTransaction().createFromUnsignedTxIO(ustxiAccum, dtxoAccum)
      



################################################################################
def computePromissoryID(ustxiList=None, dtxoTarget=None, feeAmt=None, 
                        dtxoChange=None, prom=None):

   if prom:
      ustxiList  = prom.ustxInputs
      dtxoTarget = prom.dtxoTarget
      feeAmt     = prom.feeAmt
      dtxoChange = prom.dtxoChange

   if not ustxiList:
      LOGERROR("Empty ustxiList in computePromissoryID")
      return None

   outptList = sorted([ustxi.outpoint.serialize() for ustxi in ustxiList])
   targStr  = dtxoTarget.binScript 
   targStr += int_to_binary(dtxoTarget.value, widthBytes=8)
   targStr += dtxoChange.binScript
   return binary_to_base58(hash256(''.join(outptList) + targStr))[:8]
   


################################################################################
################################################################################
class MultiSigPromissoryNote(object):


   #############################################################################
   def __init__(self, dtxoTarget=None, feeAmt=None, ustxInputs=None, 
                                    dtxoChange=None, promLabel=None,
                                    version=MULTISIG_VERSION):
      self.version     = 0
      self.dtxoTarget  = dtxoTarget
      self.feeAmt      = feeAmt
      self.ustxInputs  = ustxInputs
      self.dtxoChange  = dtxoChange
      self.promID      = None
      self.promLabel   = promLabel if promLabel else ''

      # We MIGHT use this object to simultaneously promise funds AND 
      # provide a key to include in the target multisig lockbox (which would 
      # save a round of exchanging data, if the use-case allows it)
      self.lockboxKey = ''

      if dtxoTarget is not None:
         self.setParams(dtxoTarget, feeAmt, dtxoChange, ustxInputs, 
                                                   promLabel, version)


   #############################################################################
   def setParams(self, dtxoTarget=None, feeAmt=None, dtxoChange=None,
                    ustxInputs=None, promLabel=None, version=MULTISIG_VERSION):
      
      # Set params will only overwrite with non-None data
      if dtxoTarget is not None:
         self.dtxoTarget = dtxoTarget

      if feeAmt is not None:
         self.feeAmt = feeAmt

      if dtxoChange is not None:
         self.dtxoChange = dtxoChange

      if ustxInputs is not None:
         self.ustxInputs = ustxInputs

      if promLabel is not None:
         self.promLabel = promLabel

      # Compute some other data members
      self.version = version
      self.magicBytes = MAGIC_BYTES

      self.promID = computePromissoryID(prom=self)

      # Make sure that the change output matches expected, also set contribIDs
      totalInputs = 0
      for ustxi in self.ustxInputs:
         totalInputs += ustxi.value
         ustxi.contribID = self.promID

      changeAmt = totalInputs - (self.dtxoTarget.value + self.feeAmt)
      if changeAmt > 0:
         if not self.dtxoChange.value==changeAmt:
            LOGERROR('dtxoChange.value==%s, computed %s',
               coin2strNZS(self.dtxoChange.value), coin2strNZS(changeAmt))
            raise ValueError('Change output on prom note is unexpected')
      elif changeAmt < 0:
         LOGERROR('Insufficient prom inputs for payAmt and feeAmt')
         LOGERROR('Total inputs: %s', coin2strNZS(totalInputs))
         LOGERROR('(Amt, Fee)=(%s,%s)', coin2strNZS(self.dtxoTarget.value), 
                                              coin2strNZS(self.feeAmt))
         raise ValueError('Insufficient prom inputs for pay & fee')


   #############################################################################
   def setLockboxKey(self, binPubKey):
      keyPair = [binPubKey[0], len(binPubKey)] 
      if not keyPair in [['\x02', 33], ['\x03', 33], ['\x04', 65]]:
         LOGERROR('Invalid public key supplied')
         return False
      
      if keyPair[0] == '\x04':
         if not CryptoECDSA().VerifyPublicKeyValid(SecureBinaryData(binPubKey)):
            LOGERROR('Invalid public key supplied')
            return False

      self.lockboxKey = binPubKey[:]
      return True
      
      
   #############################################################################
   def serialize(self):

      if not self.dtxoTarget:
         LOGERROR('Cannot serialize uninitialized promissory note')
         return None

      if self.dtxoChange is None:
         serChange = ''
      else:
         serChange = self.dtxoChange.serialize()

      bp = BinaryPacker()
      bp.put(UINT32,       self.version)
      bp.put(BINARY_CHUNK, MAGIC_BYTES)
      bp.put(VAR_STR,      self.dtxoTarget.serialize())
      bp.put(VAR_STR,      serChange)
      bp.put(UINT64,       self.feeAmt)
      bp.put(VAR_INT,      len(self.ustxInputs))
      for ustxi in self.ustxInputs:
         bp.put(VAR_STR,      ustxi.serialize())

      bp.put(VAR_STR,      toBytes(self.promLabel))
      bp.put(VAR_STR,      self.lockboxKey)

      return bp.getBinaryString()

   #############################################################################
   def unserialize(self, rawData, expectID=None):
      ustxiList = []
      
      bu = BinaryUnpacker(rawData)
      version     = bu.get(UINT32)
      magicBytes  = bu.get(BINARY_CHUNK, 4)
      target      = bu.get(VAR_STR)
      change      = bu.get(VAR_STR)
      feeAmt      = bu.get(UINT64)
      numUSTXI    = bu.get(VAR_INT)
      
      for i in range(numUSTXI):
         ustxiList.append( UnsignedTxInput().unserialize(bu.get(VAR_STR)) )

      promLabel   = toUnicode(bu.get(VAR_STR))
      lockboxKey  = bu.get(VAR_STR)

      if not version==MULTISIG_VERSION:
         LOGWARN('Unserialing promissory note of different version')
         LOGWARN('   PromNote Version: %d' % version)
         LOGWARN('   Armory   Version: %d' % MULTISIG_VERSION)

      dtxoTarget = DecoratedTxOut().unserialize(target)
      dtxoChange = DecoratedTxOut().unserialize(change) if change else None

      self.setParams(dtxoTarget, feeAmt, dtxoChange, ustxiList, promLabel)

      if expectID and not expectID==self.promID:
         LOGERROR('Promissory note ID does not match expected')
         return None

      if len(lockboxKey)>0:
         self.setLockboxKey(lockboxKey)
      

      return self


   #############################################################################
   def serializeAscii(self, wid=80, newline='\n'):
      headStr = 'PROMISSORY-%s' % self.promID
      return makeAsciiBlock(self.serialize(), headStr, wid, newline)


   #############################################################################
   def unserializeAscii(self, promBlock):

      headStr, rawData = readAsciiBlock(promBlock, 'PROMISSORY')

      if rawData is None:
         LOGERROR('Expected header str "PROMISSORY", got "%s"' % headStr)
         raise UnserializeError('Expected PROMISSORY block, got something else')

      # We should have "PROMISSORY-PROMID" in the headstr
      promID = headStr.split('-')[-1]
      return self.unserialize(rawData, promID)


   #############################################################################
   def pprint(self):

      #self.lockboxKey     = ''
      #self.lockboxKeyInfo = ''
      print 'Promissory Note:'
      print '   Version     :', self.version
      print '   Unique ID   :', self.promID
      print '   Num Inputs  :', len(self.ustxInputs)
      print '   Target Addr :', self.dtxoTarget.getRecipStr()
      print '   Pay Amount  :', self.dtxoTarget.value
      print '   Fee Amount  :', self.feeAmt
      if self.dtxoChange is not None:
         print '   ChangeAddr  :', self.dtxoChange.getRecipStr()
      print '   LB Key      :', self.lockboxKey
      print '   LB Key Info :', self.promLabel
















