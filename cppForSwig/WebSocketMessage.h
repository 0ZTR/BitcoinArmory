////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2018, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _WEBSOCKET_MESSAGE_H
#define _WEBSOCKET_MESSAGE_H

#include <stdexcept>
#include <string>

#include "BinaryData.h"

#define WEBSOCKET_MESSAGE_PACKET_SIZE 8000
#define WEBSOCKET_MESSAGE_PACKET_HEADER 10
#define WEBSOCKET_CALLBACK_ID 0xFFFFFFFFFFFFFFFE

using namespace std;

class LWS_Error : public runtime_error
{
public:
   LWS_Error(const string& err) :
      runtime_error(err)
   {}
};

class WebSocketMessage
{
private:
   map<uint8_t, BinaryData> packets_;
   uint64_t id_ = UINT64_MAX;
   unsigned count_ = UINT32_MAX;

public:
   static vector<BinaryData> serialize(uint64_t messageID, const string& message);
   static uint64_t getMessageId(const BinaryData&);

   void processPacket(BinaryData&);
   bool reconstruct(string& msg);
   uint64_t id(void) const { return id_; }
};

#endif
