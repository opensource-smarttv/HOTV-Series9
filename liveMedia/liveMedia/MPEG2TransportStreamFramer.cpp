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
// A filter that passes through (unchanged) chunks that contain an integral number
// of MPEG-2 Transport Stream packets, but returning (in "fDurationInMicroseconds")
// an updated estimate of the time gap between chunks.
// Implementation

#include "MPEG2TransportStreamFramer.hh"
#include <GroupsockHelper.hh> // for "gettimeofday()"
#include "BasicTCPSource.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>


#define TRANSPORT_PACKET_SIZE 188

////////// Definitions of constants that control the behavior of this code /////////

#if !defined(NEW_DURATION_WEIGHT)
#define NEW_DURATION_WEIGHT 0.5
// How much weight to give to the latest duration measurement (must be <= 1)
#endif

#if !defined(TIME_ADJUSTMENT_FACTOR)
#define TIME_ADJUSTMENT_FACTOR 0.8
// A factor by which to adjust the duration estimate to ensure that the overall
// packet transmission times remains matched with the PCR times (which will be the
// times that we expect receivers to play the incoming packets).
// (must be <= 1)
#endif

#if !defined(MAX_PLAYOUT_BUFFER_DURATION)
#define MAX_PLAYOUT_BUFFER_DURATION 0.1 // (seconds)
#endif

#if !defined(PCR_PERIOD_VARIATION_RATIO)
#define PCR_PERIOD_VARIATION_RATIO 0.5
#endif

////////// PIDStatus //////////

class PIDStatus
{
public:
    PIDStatus(double _firstClock, double _firstRealTime)
        : firstClock(_firstClock), lastClock(_firstClock),
          firstRealTime(_firstRealTime), lastRealTime(_firstRealTime),
          lastPacketNum(0)
    {
    }

    double firstClock, lastClock, firstRealTime, lastRealTime;
    unsigned lastPacketNum;
};


////////// MPEG2TransportStreamFramer //////////

MPEG2TransportStreamFramer* MPEG2TransportStreamFramer
::createNew(UsageEnvironment& env, FramedSource* inputSource,Boolean is_wfd)
{
    return new MPEG2TransportStreamFramer(env, inputSource,is_wfd);
}

MPEG2TransportStreamFramer
::MPEG2TransportStreamFramer(UsageEnvironment& env, FramedSource* inputSource, Boolean is_wfd)
    : FramedFilter(env, inputSource),
      fTSPacketCount(0), fTSPacketDurationEstimate(0.0), fTSPCRCount(0),
	fLimitNumTSPacketsToStream(False), fNumTSPacketsToStream(0), is_for_wfd(is_wfd), fTransport(0), fUDPSource(inputSource)
{
    fPIDStatusTable = HashTable::create(ONE_WORD_HASH_KEYS);
}

MPEG2TransportStreamFramer::~MPEG2TransportStreamFramer()
{
    clearPIDStatusTable();
    delete fPIDStatusTable;
}

void MPEG2TransportStreamFramer::clearPIDStatusTable()
{
    PIDStatus* pidStatus;
    while ((pidStatus = (PIDStatus*)fPIDStatusTable->RemoveNext()) != NULL)
    {
        delete pidStatus;
    }
}

void MPEG2TransportStreamFramer::setNumTSPacketsToStream(unsigned long numTSRecordsToStream)
{
    fNumTSPacketsToStream = numTSRecordsToStream;
    fLimitNumTSPacketsToStream = numTSRecordsToStream > 0;
}

Boolean MPEG2TransportStreamFramer::switchTransport(int port, int fTCP)
{
	if (fTransport == fTCP)
	{
		printf("Don't need to switchTransport \n");
		return False;
	}
	fTransport = fTCP;
	
	fInputSource->SetCurrentlyAwaitingData(False);
	if (fTransport)
	{
		fInputSource = BasicTCPSource::createNew(envir(), port);
	}
	else
	{
		Medium::close(fInputSource);
		fInputSource = fUDPSource;
	}
	FramedFilter::SetinputSource(fInputSource);
	fInputSource->SetCurrentlyAwaitingData(False);
	return True;
}

void MPEG2TransportStreamFramer::doGetNextFrame()
{
    if (fLimitNumTSPacketsToStream)
    {
        if (fNumTSPacketsToStream == 0)
        {
            handleClosure(this);
            return;
        }
        if (fNumTSPacketsToStream*TRANSPORT_PACKET_SIZE < fMaxSize)
        {
            fMaxSize = fNumTSPacketsToStream*TRANSPORT_PACKET_SIZE;
        }
    }

    // Read directly from our input source into our client's buffer:
    fFrameSize = 0;
    fInputSource->getNextFrame(fTo, fMaxSize,
                               afterGettingFrame, this,
                               FramedSource::handleClosure, this);
}

void MPEG2TransportStreamFramer::doStopGettingFrames()
{
    FramedFilter::doStopGettingFrames();
    fTSPacketCount = 0;
    fTSPCRCount = 0;

    clearPIDStatusTable();
}

void MPEG2TransportStreamFramer
::afterGettingFrame(void* clientData, unsigned frameSize,
                    unsigned /*numTruncatedBytes*/,
                    struct timeval presentationTime,
                    unsigned /*durationInMicroseconds*/)
{
    MPEG2TransportStreamFramer* framer = (MPEG2TransportStreamFramer*)clientData;
    framer->afterGettingFrame1(frameSize, presentationTime);
}

#define TRANSPORT_SYNC_BYTE 0x47

void MPEG2TransportStreamFramer::afterGettingFrame1(unsigned frameSize,
        struct timeval presentationTime)
{
//#define DUMP_DATA     
#ifdef DUMP_DATA
    static int fd1 = -1;
    static int fd2 = -1;
    if(fd1 == -1)
    {
        fd1 = ::open("/3rd/stream98", O_RDWR | O_CREAT | O_TRUNC | O_APPEND);
    }
    if(fd2 == -1)
    {
        fd2 = ::open("/3rd/stream99", O_RDWR | O_CREAT | O_TRUNC | O_APPEND);
    }
    if (fd1 != -1)
        ::write(fd1, fTo, frameSize);
#endif
//#define DEBUG_INFO     
#ifdef DEBUG_INFO
    static unsigned int enterTimes = 0;
    enterTimes++;
    if(enterTimes % 30 == 0)
    {
        printf("time %u \n", enterTimes);
//        printf("MPEG2TransportStreamFramer::afterGettingFrame1 (line %d, time %u) frameSize = %u, fFrameSize = %u \n", __LINE__, enterTimes, frameSize, fFrameSize);
    }
#endif
    fFrameSize += frameSize;
#if 0
    unsigned const numTSPackets = fFrameSize/TRANSPORT_PACKET_SIZE;
    fNumTSPacketsToStream -= numTSPackets;
    fFrameSize = numTSPackets*TRANSPORT_PACKET_SIZE; // an integral # of TS packets
    if (fFrameSize == 0)
    {
        // We didn't read a complete TS packet; assume that the input source has closed.
        //handleClosure(this);
        //return;
    }

    // Make sure the data begins with a sync byte:
    unsigned syncBytePosition;
    for (syncBytePosition = 0; syncBytePosition < fFrameSize; ++syncBytePosition)
    {
//        if (fTo[syncBytePosition] == TRANSPORT_SYNC_BYTE) break;
        if ((fTo[syncBytePosition] == TRANSPORT_SYNC_BYTE) && (syncBytePosition >= frameSize - fFrameSize))
            break; 
    }

    if (syncBytePosition == fFrameSize)
    {
        envir() << "No Transport Stream sync byte in data.";
        //handleClosure(this);
        //return;
    }
    else if (syncBytePosition > 0)
    {
        // There's a sync byte, but not at the start of the data.  Move the good data
        // to the start of the buffer, then read more to fill it up again:
#ifdef DEBUG_INFO2
        printf("(line %d) frameSize = %u, fFrameSize = %u, syncBytePosition = %u \n",
               __LINE__, frameSize, fFrameSize, syncBytePosition);
#endif

#ifdef DEBUG_INFO2
        printf("fTo[0] -- fTo[31]: (Hex) \n");
        for(unsigned i = 0; i < 32; i++)
        {
            printf("%02x ", fTo[i]);
            if(i == 15)
            {
                printf("\n");
            }
        }
        printf("\n");
        printf("fTo[-16] -- fTo[-1]: (Hex) \n");
        for(unsigned j = frameSize - 16; j < frameSize; j++)
        {
            printf("%02x ", fTo[j]);
        }
        printf("\n");
#endif

#if 0
        memmove(fTo, &fTo[syncBytePosition], fFrameSize - syncBytePosition);
        fFrameSize -= syncBytePosition;
        fInputSource->getNextFrame(&fTo[fFrameSize], syncBytePosition,
                                   afterGettingFrame, this,
                                   FramedSource::handleClosure, this);
        return;
#else
/*
        memmove(fTo, &fTo[syncBytePosition], frameSize - syncBytePosition);
        frameSize -= syncBytePosition;
        fInputSource->getNextFrame(&fTo[fFrameSize], syncBytePosition,
                                   afterGettingFrame, this,
                                   FramedSource::handleClosure, this);
        return;
*/        
    //    memmove(fTo, &fTo[syncBytePosition], fFrameSize);
#endif
    } // else normal case: the data begins with a sync byte

#ifdef DUMP_DATA
    if (fd2 != -1)
        ::write(fd2, fTo, fFrameSize);
#endif

    fPresentationTime = presentationTime;

    // Scan through the TS packets that we read, and update our estimate of
    // the duration of each packet:
    struct timeval tvNow;
    gettimeofday(&tvNow, NULL);
    double timeNow = tvNow.tv_sec + tvNow.tv_usec/1000000.0;
    for (unsigned i = 0; i < numTSPackets; ++i)
    {
        updateTSPacketDurationEstimate(&fTo[i*TRANSPORT_PACKET_SIZE], timeNow);
    }

    fDurationInMicroseconds
    = numTSPackets * (unsigned)(fTSPacketDurationEstimate*1000000);
#endif
    // Complete the delivery to our client:
    afterGetting(this);
}

void MPEG2TransportStreamFramer
::updateTSPacketDurationEstimate(unsigned char* pkt, double timeNow)
{
    // Sanity check: Make sure we start with the sync byte:
    if (pkt[0] != TRANSPORT_SYNC_BYTE)
    {
        /* Commented by MTK. WFD does not have sync byte. */
        //envir() << "Missing sync byte!\n";
        return;
    }

    ++fTSPacketCount;

    // If this packet doesn't contain a PCR, then we're not interested in it:
    u_int8_t const adaptation_field_control = (pkt[3]&0x30)>>4;
    if (adaptation_field_control != 2 && adaptation_field_control != 3) return;
    // there's no adaptation_field

    u_int8_t const adaptation_field_length = pkt[4];
    if (adaptation_field_length == 0) return;

    u_int8_t const discontinuity_indicator = pkt[5]&0x80;
    u_int8_t const pcrFlag = pkt[5]&0x10;
    if (pcrFlag == 0) return; // no PCR

    // There's a PCR.  Get it, and the PID:
    ++fTSPCRCount;
    u_int32_t pcrBaseHigh = (pkt[6]<<24)|(pkt[7]<<16)|(pkt[8]<<8)|pkt[9];
    double clock = pcrBaseHigh/45000.0;
    if ((pkt[10]&0x80) != 0) clock += 1/90000.0; // add in low-bit (if set)
    unsigned short pcrExt = ((pkt[10]&0x01)<<8) | pkt[11];
    clock += pcrExt/27000000.0;

    unsigned pid = ((pkt[1]&0x1F)<<8) | pkt[2];

    // Check whether we already have a record of a PCR for this PID:
    PIDStatus* pidStatus = (PIDStatus*)(fPIDStatusTable->Lookup((char*)pid));

    if (pidStatus == NULL)
    {
        // We're seeing this PID's PCR for the first time:
        pidStatus = new PIDStatus(clock, timeNow);
        fPIDStatusTable->Add((char*)pid, pidStatus);
#ifdef DEBUG_PCR
//    fprintf(stderr, "PID 0x%x, FIRST PCR 0x%08x+%d:%03x == %f @ %f, pkt #%lu\n", pid, pcrBaseHigh, pkt[10]>>7, pcrExt, clock, timeNow, fTSPacketCount);
#endif
    }
    else
    {
        // We've seen this PID's PCR before; update our per-packet duration estimate:
        double durationPerPacket
        = (clock - pidStatus->lastClock)/(fTSPacketCount - pidStatus->lastPacketNum);

        // Hack (suggested by "Romain"): Don't update our estimate if this PCR appeared unusually quickly.
        // (This can produce more accurate estimates for wildly VBR streams.)
        double meanPCRPeriod = 0.0;
        if (fTSPCRCount > 0)
        {
            meanPCRPeriod = (double)fTSPacketCount/fTSPCRCount;
            if (fTSPacketCount - pidStatus->lastPacketNum < meanPCRPeriod*PCR_PERIOD_VARIATION_RATIO) return;
        }

        if (fTSPacketDurationEstimate == 0.0)   // we've just started
        {
            fTSPacketDurationEstimate = durationPerPacket;
        }
        else if (discontinuity_indicator == 0 && durationPerPacket >= 0.0)
        {
            fTSPacketDurationEstimate
            = durationPerPacket*NEW_DURATION_WEIGHT
              + fTSPacketDurationEstimate*(1-NEW_DURATION_WEIGHT);

            // Also adjust the duration estimate to try to ensure that the transmission
            // rate matches the playout rate:
            double transmitDuration = timeNow - pidStatus->firstRealTime;
            double playoutDuration = clock - pidStatus->firstClock;
            if (transmitDuration > playoutDuration)
            {
                fTSPacketDurationEstimate *= TIME_ADJUSTMENT_FACTOR; // reduce estimate
            }
            else if (transmitDuration + MAX_PLAYOUT_BUFFER_DURATION < playoutDuration)
            {
                fTSPacketDurationEstimate /= TIME_ADJUSTMENT_FACTOR; // increase estimate
            }
        }
        else
        {
            // the PCR has a discontinuity from its previous value; don't use it now,
            // but reset our PCR and real-time values to compensate:
            pidStatus->firstClock = clock;
            pidStatus->firstRealTime = timeNow;
        }
#ifdef DEBUG_PCR
//    fprintf(stderr, "PID 0x%x, PCR 0x%08x+%d:%03x == %f @ %f (diffs %f @ %f), pkt #%lu, discon %d => this duration %f, new estimate %f, mean PCR period=%f\n", pid, pcrBaseHigh, pkt[10]>>7, pcrExt, clock, timeNow, clock - pidStatus->firstClock, timeNow - pidStatus->firstRealTime, fTSPacketCount, discontinuity_indicator != 0, durationPerPacket, fTSPacketDurationEstimate, meanPCRPeriod );
#endif
    }

    pidStatus->lastClock = clock;
    pidStatus->lastRealTime = timeNow;
    pidStatus->lastPacketNum = fTSPacketCount;
}
