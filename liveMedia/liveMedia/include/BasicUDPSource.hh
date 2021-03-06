/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// "liveMedia"
// Copyright (c) 1996-2011 Live Networks, Inc.  All rights reserved.
// A simple UDP source, where every UDP payload is a complete frame
// C++ header

#ifndef _BASIC_UDP_SOURCE_HH
#define _BASIC_UDP_SOURCE_HH

#ifndef _FRAMED_SOURCE_HH
#include "FramedSource.hh"
#endif
#ifndef _GROUPSOCK_HH
#include "Groupsock.hh"
#endif

#define USHORT_MAX 65535
#define MAX_READ_COUNT		10	
#define RTP_HEADER_LEN		12

typedef struct rtp_header_len_s
{
	char buf[RTP_HEADER_LEN];
}rtp_header_len_t;

class BasicUDPSource: public FramedSource {
public:
  static BasicUDPSource* createNew(UsageEnvironment& env, Groupsock* inputGS);

  virtual ~BasicUDPSource();

  Groupsock* gs() const { return fInputGS; }

private:
  BasicUDPSource(UsageEnvironment& env, Groupsock* inputGS);
      // called only by createNew()

  static void incomingPacketHandler(BasicUDPSource* source, int mask);
  void incomingPacketHandler1();

private: // redefined virtual functions:
  virtual void doGetNextFrame();
  virtual void doStopGettingFrames();

private:
  Groupsock* fInputGS;
  Boolean fHaveStartedReading;
  Boolean fHaveSeenFirstPacket; 
  unsigned fReadSize;
  unsigned short fReadCount;
  unsigned short fNextExpectedSeqNo;
};

#endif
