/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <array>
#include "Logging.h"
#include "MozQuic.h"
#include "MozQuicInternal.h"
#include "NSSHelper.h"
#include "Sender.h"
#include "Streams.h"
#include "Timer.h"
#include "TransportExtension.h"

#include "assert.h"
#include "netinet/ip.h"
#include "stdlib.h"
#include "unistd.h"
#include "time.h"
#include "sys/time.h"
#include <string.h>
#include <fcntl.h>
#include "prerror.h"
#include <sys/socket.h>
#include <netinet/in.h>

namespace mozquic  {

const char *MozQuic::kAlpn = MOZQUIC_ALPN;
  
static const uint16_t kIdleTimeoutDefault = 600;
static const int kTargetUDPBuffer = 16 * 1024 * 1024;

MozQuic::MozQuic(bool handleIO)
  : mFD(MOZQUIC_SOCKET_BAD)
  , mHandleIO(handleIO)
  , mIsClient(true)
  , mIsChild(false)
  , mReceivedServerClearText(false)
  , mSetupTransportExtension(false)
  , mIgnorePKI(false)
  , mTolerateBadALPN(false)
  , mTolerateNoTransportParams(false)
  , mSabotageVN(false)
  , mForceAddressValidation(false)
  , mAppHandlesSendRecv(false)
  , mAppHandlesLogging(false)
  , mIsLoopback(false)
  , mProcessedVN(false)
  , mBackPressure(false)
  , mEnabled0RTT(false)
  , mReject0RTTData(false)
  , mIPV6(false)
  , mProcessed0RTT(false)
  , mConnectionState(STATE_UNINITIALIZED)
  , mOriginPort(-1)
  , mClientPort(-1)
//  , mVersion(kMozQuicVersion1)
  , mVersion(kMozQuicIetfID9)
  , mClientOriginalOfferedVersion(0)
  , mMaxPacketConfig(kDefaultMaxPacketConfig)
  , mMTU(kInitialMTU)
  , mDropRate(0)
  , mConnectionID(0)
  , mOriginalConnectionID(0)
  , mNextTransmitPacketNumber(0)
  , mOriginalTransmitPacketNumber(0)
  , mNextRecvPacketNumber(0)
  , mClientInitialPacketNumber(0)
  , mGenAckFor(0)
  , mGenAckForTime(0)
  , mDelAckTimer(new Timer(this))
  , mClosure(nullptr)
  , mConnEventCB(nullptr)
  , mParent(nullptr)
  , mAlive(this)
  , mTimestampConnBegin(0)
  , mPingDeadline(new Timer(this))
  , mPMTUD1Deadline(new Timer(this))
  , mPMTUD1PacketNumber(0)
  , mPMTUDTarget(kMaxMTU)
  , mIdleDeadline(new Timer(this))
  , mDecodedOK(false)
  , mLocalOmitCID(false)
  , mPeerOmitCID(false)
  , mPeerIdleTimeout(kIdleTimeoutDefault)
  , mPeerAckDelayExponent(kDefaultAckDelayExponent)
  , mLocalAckDelayExponent(10)
  , mAdvertiseStreamWindow(kMaxStreamDataDefault)
  , mAdvertiseConnectionWindow(kMaxDataDefault)
  , mLocalMaxSizeAllowed(0)
  , mRemoteTransportExtensionInfoLen(0)
  , mCheck0RTTPossible(false)
  , mEarlyDataState(EARLY_DATA_NOT_NEGOTIATED)
  , mEarlyDataLastPacketNumber(0)
  , mHighestTransmittedAckable(0)
{
  Log::sParseSubscriptions(getenv("MOZQUIC_LOG"));
  
  assert(!handleIO); // todo
  unsigned char seed[4];
  if (SECSuccess != PK11_GenerateRandom(seed, sizeof(seed))) {
    // major badness!
    srandom(Timestamp() & 0xffffffff);
  } else {
    srandom(seed[0] << 24 | seed[1] << 16 | seed[2] << 8 | seed[3]);
  }
  memset(&mPeer, 0, sizeof(mPeer));
  memset(mStatelessResetKey, 0, sizeof(mStatelessResetKey));
  memset(mStatelessResetToken, 0x80, sizeof(mStatelessResetToken));
  mSendState.reset(new Sender(this));
}

MozQuic::~MozQuic()
{
  if (!mIsChild && (mFD != MOZQUIC_SOCKET_BAD)) {
    close(mFD);
  }
}

void
MozQuic::Alarm(Timer *timer)
{
  if (timer == mDelAckTimer.get()) {
    MaybeSendAck(false);
  } else if (timer == mPingDeadline.get()) {
    if (mConnEventCB) {
      ConnectionLog1("ping deadline expired\n");
      mConnEventCB(mClosure, MOZQUIC_EVENT_ERROR, this);
    }
  } else if (timer == mIdleDeadline.get()) {
    if (mConnectionState == CLIENT_STATE_CONNECTED ||
        mConnectionState == SERVER_STATE_CONNECTED) {
      RaiseError(MOZQUIC_ERR_GENERAL, (char *)"Idle Timeout");
    }
  } else if (timer == mPMTUD1Deadline.get()) {
    AbortPMTUD1();
  } else {
    assert(0);
  }
}

bool
MozQuic::IsAllAcked()
{
  return mStreamState ? mStreamState->IsAllAcked() : true;
}

void
MozQuic::Destroy(uint32_t code, const char *reason)
{
  Shutdown(code, reason);
  mAlive = nullptr;
}

uint32_t
MozQuic::FlushOnce(bool forceAck, bool forceFrame)
{
  if (!mStreamState) {
    return MOZQUIC_ERR_GENERAL;
  }
  bool didWrite;
  return mStreamState->FlushOnce(forceAck, forceFrame, didWrite);
}

uint32_t
MozQuic::RetransmitOldestUnackedData(bool fromRTO)
{
  // objective is to get oldest unacked data into Sender::mQueue

  // streamstate::retransmitoldestunackeddata -> ss::connectionwritenow()
  // .. puts data in mConnUnwritten (at front)
  // .. calls flush()
  // ss::flush dequeues from mConnUnWritten and quic frames it
  // .. then puts it on mUnackedPackets with fromRTO tag
  // .. then calls protectedTransmit()
  // mq::protectedTransmit encrypts and calls transmit()
  // transmit can't send it out yet (blocked on cwnd) and leaves in mQueue

  if (mStreamState) {
    mStreamState->RetransmitOldestUnackedData(fromRTO);
  }
  return MOZQUIC_OK;
}

bool
MozQuic::AnyUnackedPackets()
{
  if (mStreamState) {
    return mStreamState->AnyUnackedPackets();
  }
  return false;
}
 
uint32_t
MozQuic::RealTransmit(const unsigned char *pkt, uint32_t len, const struct sockaddr *explicitPeer,
                      bool updateTimers)
{
  // should only be called by 'sender' class after pacing and cong control conditions met.

  mIdleDeadline->Arm(mPeerIdleTimeout * 1000);

  if (updateTimers) {
    mSendState->EstablishPTOTimer();
  }

  static bool one = true;
  if (mDropRate && ((random() % 100) <  mDropRate)) {
    one = false;
    ConnectionLog2("Transmit dropped due to drop rate\n");
    return MOZQUIC_OK;
  }

  if (mAppHandlesSendRecv) {
    struct mozquic_eventdata_transmit data;
    data.pkt = pkt;
    data.len = len;
    data.explicitPeer = explicitPeer;
    return mConnEventCB(mClosure, MOZQUIC_EVENT_TRANSMIT, &data);
  }

  int rv;
  if (mIsChild || explicitPeer) {
    const struct sockaddr *peer = explicitPeer ? explicitPeer : (const struct sockaddr *) &mPeer;
    socklen_t sop = mIPV6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    rv = sendto(mFD, pkt, len, 0, peer, sop);
  } else {
    rv = send(mFD, pkt, len, 0);
  }

  if (rv == -1) {
    ConnectionLog1("Sending error in transmit\n");
  }

  return MOZQUIC_OK;
}

uint32_t
MozQuic::ProtectedTransmit(unsigned char *header, uint32_t headerLen,
                           unsigned char *data, uint32_t dataLen, uint32_t dataAllocation,
                           bool addAcks, bool ackable, bool queueOnly, uint32_t MTU, uint32_t *bytesOut)
{
  bool bareAck = dataLen == 0;
  if (bytesOut) {
    *bytesOut = 0;
  }
  if (!MTU) {
    MTU = mMTU;
  }

  if (mNextTransmitPacketNumber >= ((1ULL << 62) - 1)) {
    ConnectionLog1("Connection Packet Number Exhausted\n");
    RaiseError(MOZQUIC_ERR_GENERAL, "Connection Packet Number Exhausted\n");
    return MOZQUIC_ERR_GENERAL;
  }

  // if ack info has not changed, only send it 2xrtt
  if (addAcks && !queueOnly &&
      (mGenAckFor == mNextRecvPacketNumber) &&
      ((Timestamp() - mGenAckForTime) < (mSendState->SmoothedRTT() >> 1))) {
    addAcks = false;
    AckLog6("redundant ack suppressed\n");
  }

  if (addAcks) {
    uint32_t room = MTU - kTagLen - headerLen - dataLen;
    if (room > dataAllocation) {
      room = dataAllocation;
    }
    uint32_t usedByAck = 0;
    if (AckPiggyBack(data + dataLen, mNextTransmitPacketNumber, room, keyPhase1Rtt, bareAck, usedByAck) == MOZQUIC_OK) {
      if (usedByAck) {
        AckLog6("Handy-Ack adds to protected Transmit packet %lX by %d\n", mNextTransmitPacketNumber, usedByAck);
        dataLen += usedByAck;
        mGenAckFor = mNextRecvPacketNumber;
        mGenAckForTime = Timestamp();
        mDelAckTimer->Cancel();
      }
    }
  }

  if (dataLen == 0) {
    ConnectionLog6("nothing to write\n");
    return MOZQUIC_OK;
  }

  uint32_t written = 0;
  unsigned char cipherPkt[kMaxMTU];
  memcpy(cipherPkt, header, headerLen);
  assert(headerLen + dataLen + 16 <= MTU);
  uint32_t rv = 0;
  if (mConnectionState == CLIENT_STATE_0RTT) {
    rv = mNSSHelper->EncryptBlock0RTT(header, headerLen, data, dataLen,
                                      mNextTransmitPacketNumber, cipherPkt + headerLen,
                                      kMaxMTU, written);
  } else {
    rv = mNSSHelper->EncryptBlock(header, headerLen, data, dataLen,
                                  mNextTransmitPacketNumber, cipherPkt + headerLen,
                                  kMaxMTU, written);
  }

  assert(!headerLen); // dtls hack
  if (written < 11) {
    ConnectionLog1("DTLS HEADER NOT AT LEAST 11 TRANSMIT\n");
    assert(0);
    return MOZQUIC_ERR_CRYPTO;
  }
  mNextTransmitPacketNumber = 0;
  memcpy(((unsigned char *)(&mNextTransmitPacketNumber)), cipherPkt + 3, 8);
  mNextTransmitPacketNumber = PR_ntohll(mNextTransmitPacketNumber);
  
  ConnectionLog6("encrypt[%lX] rv=%d inputlen=%d (+%d of aead) outputlen=%d\n",
                 mNextTransmitPacketNumber, rv, dataLen, headerLen, written);

  if (rv != MOZQUIC_OK) {
    RaiseError(MOZQUIC_ERR_CRYPTO, (char *) "unexpected encrypt fail");
    return rv;
  }

  rv = mSendState->Transmit(mNextTransmitPacketNumber, bareAck,
                            mConnectionState == CLIENT_STATE_0RTT,
                            queueOnly,
                            cipherPkt, written + headerLen, nullptr);
  if (rv != MOZQUIC_OK) {
    return rv;
  }
  if (bytesOut) {
    *bytesOut = written + headerLen;
  }

  if (ackable) {
    assert(mHighestTransmittedAckable <= mNextTransmitPacketNumber);
    mHighestTransmittedAckable = mNextTransmitPacketNumber;
  }

  ConnectionLog5("TRANSMIT[%lX] this=%p len=%d byte0=%X ackable=%d\n",
                 mNextTransmitPacketNumber, this, written + headerLen,
                 header[0], ackable);
  mNextTransmitPacketNumber++;
  
  return MOZQUIC_OK;
}

void
MozQuic::Shutdown(uint16_t code, const char *reason)
{
  if (mParent) {
    for (auto iter = mParent->mChildren.begin(); iter != mParent->mChildren.end(); ++iter) {
      if ((*iter).get() == this) {
          mParent->mChildren.erase(iter);
          break;
      }
    }
    assert(mIsChild);
    mParent->RemoveSession((const sockaddr *)&mPeer);
  }

  if ((mConnectionState != CLIENT_STATE_CONNECTED) &&
      (mConnectionState != SERVER_STATE_CONNECTED)) {
    mConnectionState = mIsClient ? CLIENT_STATE_CLOSED : SERVER_STATE_CLOSED;
    return;
  }
  if (!mIsChild && !mIsClient) {
    // this is the listener.. it does not send packets
    return;
  }

  ConnectionLog5("sending shutdown as %lX\n", mNextTransmitPacketNumber);

  unsigned char plainPkt[kMaxMTU];
  uint16_t tmp16;
  assert(mMTU <= kMaxMTU);

  // todo before merge - this can't be inlined here
  // what if not kp 0 TODO
  // todo when transport params allow truncate id, the connid might go
  // short header with connid kp = 0, 4 bytes of packetnumber
  uint32_t used, headerLen;
  CreateShortPacketHeader(plainPkt, mMTU - kTagLen, used);
  headerLen = used;

  plainPkt[used] = FRAME_TYPE_CONN_CLOSE;
  used++;
  tmp16 = htonl(code);
  memcpy(plainPkt + used, &tmp16, 2);
  used += 2;

  size_t reasonLen = strlen(reason);
  if (reasonLen > (mMTU - kTagLen - used - 2)) {
    reasonLen = mMTU - kTagLen - used - 2;
  }
  uint32_t vUsed = 0;
  EncodeVarint(reasonLen, plainPkt + used, 8, vUsed);
  used += vUsed;
  
  if (reasonLen) {
    memcpy(plainPkt + used, reason, reasonLen);
    used += reasonLen;
  }

  ProtectedTransmit(plainPkt, headerLen, plainPkt + headerLen, used - headerLen,
                    mMTU - headerLen - kTagLen, false, true);
  mConnectionState = mIsClient ? CLIENT_STATE_CLOSED : SERVER_STATE_CLOSED;
}

void
MozQuic::ReleaseBackPressure()
{
  // release id
  mBackPressure = false;
  if (mStreamState) {
    mStreamState->MaybeIssueFlowControlCredit();
  }
}

void
MozQuic::SetInitialPacketNumber()
{
  mHighestTransmittedAckable = 0;
  mNextTransmitPacketNumber = 0;
  for (int i=0; i < 2; i++) {
    mNextTransmitPacketNumber = mNextTransmitPacketNumber << 16;
    mNextTransmitPacketNumber |= random() & 0xffff;
  }
  mNextTransmitPacketNumber &= 0xffffffff; // 32 bits
  mOriginalTransmitPacketNumber = mNextTransmitPacketNumber;
  if (mNextTransmitPacketNumber > (0x100000000ULL - 1025)) {
    // small range of unacceptable values - redo
    SetInitialPacketNumber();
  }
}

int
MozQuic::StartClient()
{
  assert(!mHandleIO); // todo
  mIsClient = true;
  mLocalOmitCID = true;

  mConnectionState = CLIENT_STATE_1RTT;
  // dtls v1 hack
#if 0 
  for (int i=0; i < 4; i++) {
    mConnectionID = mConnectionID << 16;
    mConnectionID = mConnectionID | (random() & 0xffff);
  }
#endif
  mOriginalConnectionID = mConnectionID;
  SetInitialPacketNumber();

  mStreamState.reset(new StreamState(this, mAdvertiseStreamWindow, mAdvertiseConnectionWindow));
  mStreamState->InitIDs(4, 2, 1, 3, kMaxStreamIDServerDefaultBidi, kMaxStreamIDServerDefaultUni);
  mNSSHelper.reset(new NSSHelper(this, mTolerateBadALPN, mOriginName.get(), true));

  assert(!mClientOriginalOfferedVersion);
  mClientOriginalOfferedVersion = mVersion;

  if (mFD == MOZQUIC_SOCKET_BAD) {
    // the application did not pass in its own fd
    struct addrinfo *outAddr;
    // todo blocking getaddrinfo
    if (getaddrinfo(mOriginName.get(), nullptr, nullptr, &outAddr) != 0) {
      return MOZQUIC_ERR_GENERAL;
    }

    if (outAddr->ai_family == AF_INET) {
      mIPV6 = false;
      mFD = socket(AF_INET, SOCK_DGRAM, 0);
      ((struct sockaddr_in *) outAddr->ai_addr)->sin_port = htons(mOriginPort);
      if ((ntohl(((struct sockaddr_in *) outAddr->ai_addr)->sin_addr.s_addr) & 0xff000000) == 0x7f000000) {
        mIsLoopback = true;
      }
    } else if (outAddr->ai_family == AF_INET6) {
      mIPV6 = true;
      mFD = socket(AF_INET6, SOCK_DGRAM, 0);
      ((struct sockaddr_in6 *) outAddr->ai_addr)->sin6_port = htons(mOriginPort);
      const void *ptr1 = &in6addr_loopback.s6_addr;
      const void *ptr2 = &((struct sockaddr_in6 *) outAddr->ai_addr)->sin6_addr.s6_addr;
      if (!memcmp(ptr1, ptr2, 16)) {
        mIsLoopback = true;
      }
    }

    if (mClientPort != -1) {
      Bind(mClientPort);
    }
    fcntl(mFD, F_SETFL, fcntl(mFD, F_GETFL, 0) | O_NONBLOCK);
#ifdef IP_PMTUDISC_DO
    int val = IP_PMTUDISC_DO;
    setsockopt(mFD, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
#endif
    connect(mFD, outAddr->ai_addr, outAddr->ai_addrlen);
    freeaddrinfo(outAddr);
  }

  AdjustBuffering();
  mTimestampConnBegin = Timestamp();
  EnsureSetupClientTransportParameters();

  return MOZQUIC_OK;
}

int
MozQuic::StartServer()
{
  assert(!mHandleIO); // todo
  mIsClient = false;
  mStreamState.reset(new StreamState(this, mAdvertiseStreamWindow, mAdvertiseConnectionWindow));
  mStreamState->InitIDs(1, 3, 4, 2, kMaxStreamIDClientDefaultBidi, kMaxStreamIDClientDefaultUni);

  StatelessResetEnsureKey();

  assert((sizeof(mValidationKey) % sizeof(uint16_t)) == 0);
  for (unsigned int i=0; i < (sizeof(mValidationKey) / sizeof (uint16_t)); i++) {
    ((uint16_t *)mValidationKey)[i] = random() & 0xffff;
  }

  mConnectionState = SERVER_STATE_LISTEN;
  int rv = Bind(mOriginPort);
  if (rv == MOZQUIC_OK) {
    AdjustBuffering();
  }
  return rv;
}

void
MozQuic::AdjustBuffering()
{
  int bufferTarget = kTargetUDPBuffer;
  setsockopt(mFD, SOL_SOCKET, SO_RCVBUF, &bufferTarget, sizeof(bufferTarget));
  bufferTarget = kTargetUDPBuffer;
  setsockopt(mFD, SOL_SOCKET, SO_SNDBUF, &bufferTarget, sizeof(bufferTarget));

  socklen_t sizeofBufferTarget = sizeof(bufferTarget);
  getsockopt(mFD, SOL_SOCKET, SO_RCVBUF, &bufferTarget, &sizeofBufferTarget);
  ConnectionLog5("receive buffers - %dKB\n", bufferTarget / 1024);

  sizeofBufferTarget = sizeof(bufferTarget);
  getsockopt(mFD, SOL_SOCKET, SO_SNDBUF, &bufferTarget, &sizeofBufferTarget);
  ConnectionLog5("send buffers - %dKB\n", bufferTarget / 1024);
}

int
MozQuic::Bind(int portno)
{
  int domain = AF_INET;
  
  if (mFD == MOZQUIC_SOCKET_BAD) {
    if (mIPV6) {
      domain = AF_INET6;
    }
    mFD = socket(domain, SOCK_DGRAM, 0); // todo v6 and non 0 addr
    fcntl(mFD, F_SETFL, fcntl(mFD, F_GETFL, 0) | O_NONBLOCK);
  }

  int rv;
  if (domain == AF_INET) {
    struct sockaddr_in sin;
    memset (&sin, 0, sizeof (sin));
    sin.sin_family = domain;
    sin.sin_port = htons(portno);
    rv = bind(mFD, (const sockaddr *)&sin, sizeof (sin));
  } else {
    int verdad = 1;
    setsockopt(mFD, IPPROTO_IPV6, IPV6_V6ONLY, &verdad, sizeof(verdad));
    struct sockaddr_in6 sin;
    memset (&sin, 0, sizeof (sin));
    sin.sin6_family = domain;
    sin.sin6_port = htons(portno);
    rv = bind(mFD, (const sockaddr *)&sin, sizeof (sin));
  }

  return (rv != -1) ? MOZQUIC_OK : MOZQUIC_ERR_IO;
}

uint64_t
MozQuic::SerializeSockaddr(const struct sockaddr_in6 *peer)
{
  return SerializeSockaddr((const sockaddr *)peer);
}

uint64_t
MozQuic::SerializeSockaddr(const sockaddr *peer)
{
  uint64_t rv = 0;

  if (mIPV6) {
    const struct sockaddr_in6 *v6ptr = (const struct sockaddr_in6 *)peer;
    assert (v6ptr->sin6_family == AF_INET6);
    uint64_t *addr = (uint64_t *) &v6ptr->sin6_addr.s6_addr;
    rv = addr[0];
    rv ^= addr[1];
    rv ^= v6ptr->sin6_port;
  } else {
    const struct sockaddr_in *v4ptr = (const struct sockaddr_in *) peer;
    assert (v4ptr->sin_family == AF_INET);
    rv = (uint32_t) v4ptr->sin_addr.s_addr;
    rv = rv << 16;
    rv |= v4ptr->sin_port;
  }
  return rv;
}

MozQuic *
MozQuic::FindSession(const struct sockaddr_in6 *peer)
{
  return FindSession((const sockaddr *)peer);
}

MozQuic *
MozQuic::FindSession(const sockaddr *peer)
{
  assert (!mIsChild);
  if (mIsClient) {
    return this;
  }

  auto i = mConnectionHash.find(SerializeSockaddr(peer));
  if (i == mConnectionHash.end()) {
    return nullptr;
  }
  return (*i).second;
}

void
MozQuic::RemoveSession(const sockaddr *peer)
{
  assert (!mIsChild);
  if (mIsClient) {
    return;
  }
  mConnectionHash.erase(SerializeSockaddr(peer));
}

void
MozQuic::EnsureSetupClientTransportParameters()
{
  if (mSetupTransportExtension) {
    return;
  }
  mSetupTransportExtension = true;
  
  ConnectionLog9("setup transport extension (client)\n");
  unsigned char te[2048];
  uint16_t teLength = 0;
  TransportExtension::
    EncodeClientTransportParameters(te, teLength, 2048,
                                    mClientOriginalOfferedVersion,
                                    mStreamState->mLocalMaxStreamData,
                                    mStreamState->mLocalMaxData,
                                    mStreamState->mLocalMaxStreamID[BIDI_STREAM],
                                    mStreamState->mLocalMaxStreamID[UNI_STREAM],
                                    kIdleTimeoutDefault, mLocalOmitCID,
                                    mLocalMaxSizeAllowed,
                                    mLocalAckDelayExponent);
  if (mAppHandlesSendRecv) {
    struct mozquic_eventdata_tlsinput data;
    data.data = te;
    data.len = teLength;
    mConnEventCB(mClosure, MOZQUIC_EVENT_TLS_CLIENT_TPARAMS, &data);
  } else {
    mNSSHelper->SetLocalTransportExtensionInfo(te, teLength);
  }
}

int
MozQuic::Client1RTT()
{
  EnsureSetupClientTransportParameters();
  assert(!mAppHandlesSendRecv); // dtls hack

  // handle server reply internally
  uint32_t code = mNSSHelper->DriveHandshake();
  if (code != MOZQUIC_OK) {
    RaiseError(code, (char *) "client 1rtt handshake failed\n");
    return code;
  }
  if (!mCheck0RTTPossible) {
    mCheck0RTTPossible = true;
    if (mNSSHelper->IsEarlyDataPossible()) {
      mEarlyDataState = EARLY_DATA_SENT;
      mConnectionState = CLIENT_STATE_0RTT;
      // Set mPeerMaxStreamID to default. TODO: set this to proper transport parameter.
      mStreamState->mPeerMaxStreamID[BIDI_STREAM] = (mIsClient)
        ? kMaxStreamIDClientDefaultBidi
        : kMaxStreamIDServerDefaultBidi;
      mStreamState->mPeerMaxStreamID[UNI_STREAM] = (mIsClient)
        ? kMaxStreamIDClientDefaultUni
        : kMaxStreamIDServerDefaultUni;

      if (mConnEventCB) {
        mConnEventCB(mClosure, MOZQUIC_EVENT_0RTT_POSSIBLE, this);
      }
    }
  }

  if (mNSSHelper->IsHandshakeComplete()) {
    return ClientConnected();
  }

  return MOZQUIC_OK;
}

int
MozQuic::Server1RTT()
{
  assert(!mIsClient && mIsChild && mParent);
  if (mAppHandlesSendRecv) {
    // todo handle app-security on server side
    // todo make sure that includes transport parameters
    assert(false);
    RaiseError(MOZQUIC_ERR_GENERAL, (char *)"need handshaker");
    return MOZQUIC_ERR_GENERAL;
  }

  if (!mSetupTransportExtension) {
    ConnectionLog9("setup transport extension (server)\n");
    unsigned char resetToken[16];
    StatelessResetCalculateToken(mParent->mStatelessResetKey,
                                 mConnectionID, resetToken); // from key and CID
  
    unsigned char te[2048];
    uint16_t teLength = 0;
    TransportExtension::
      EncodeServerTransportParameters(te, teLength, 2048,
                                      mVersion,
                                      VersionNegotiationList, sizeof(VersionNegotiationList) / sizeof (uint32_t),
                                      mStreamState->mLocalMaxStreamData,
                                      mStreamState->mLocalMaxData,
                                      mStreamState->mLocalMaxStreamID[BIDI_STREAM],
                                      mStreamState->mLocalMaxStreamID[UNI_STREAM],
                                      kIdleTimeoutDefault, mLocalOmitCID,
                                      mLocalMaxSizeAllowed,
                                      mLocalAckDelayExponent,
                                      resetToken);
    mNSSHelper->SetLocalTransportExtensionInfo(te, teLength);
    mSetupTransportExtension = true;
  }

  uint32_t code = mNSSHelper->DriveHandshake();
  if (code != MOZQUIC_OK) {
    RaiseError(code, (char *) "server 1rtt handshake failed\n");
    return code;
  }

  if (mNSSHelper->DoHRR()) {
    mNSSHelper.reset(new NSSHelper(this, mParent->mTolerateBadALPN, mParent->mOriginName.get()));
    mParent->mConnectionHash.erase(SerializeSockaddr(&mPeer));
    mConnectionState = SERVER_STATE_SSR;
    return MOZQUIC_OK;
  } else {
    if (!mCheck0RTTPossible) {
      mCheck0RTTPossible = true;
      if (0 && mNSSHelper->IsEarlyDataAcceptedServer()) { // todo dtls
        mEarlyDataState = EARLY_DATA_ACCEPTED;
        mConnectionState = SERVER_STATE_0RTT;
      } else {
        mEarlyDataState = EARLY_DATA_IGNORED;
      }
    }
  }

  if (mNSSHelper->IsHandshakeComplete()) {
    return ServerConnected();
  }
  return MOZQUIC_OK;
}

uint32_t
MozQuic::Intake(bool *partialResult)
{
  *partialResult = false;
  if (mIsChild) {
    // parent does all fd reading
    return MOZQUIC_OK;
  }
  // check state
  assert (mConnectionState == SERVER_STATE_LISTEN ||
          mConnectionState == SERVER_STATE_1RTT ||
          mConnectionState == SERVER_STATE_0RTT ||
          mConnectionState == SERVER_STATE_CLOSED ||
          mConnectionState == CLIENT_STATE_CONNECTED ||
          mConnectionState == CLIENT_STATE_1RTT ||
          mConnectionState == CLIENT_STATE_0RTT ||
          mConnectionState == CLIENT_STATE_CLOSED);
  uint32_t rv = MOZQUIC_OK;

  bool sendAck;
  do {
    unsigned char pktReal1[kMozQuicMSS];
    unsigned char *pkt = pktReal1;
    uint32_t pktSize = 0;
    sendAck = false;
    struct sockaddr_in6 peer;

    rv = Recv(pkt, kMozQuicMSS, pktSize, &peer);
    if (rv != MOZQUIC_OK || !pktSize) {
      return rv;
    }

    // dispatch to the right MozQuic class.
    std::shared_ptr<MozQuic> session(mAlive); // default
    MozQuic *tmpSession = nullptr;
      
    ShortHeaderData tmpShortHeader(this, pkt, pktSize, 0, mLocalOmitCID,
                                   mLocalOmitCID ? mConnectionID : 0);
    if (pktSize < tmpShortHeader.mHeaderSize) {
      return rv;
    }

    tmpSession = FindSession(&peer);
    if (!tmpSession) {
      ConnectionLogCID1(tmpShortHeader.mConnectionID,
                        "no session found for encoded packet pn=%lX size=%d\n",
                        tmpShortHeader.mPacketNumber, pktSize);
      if (!mIsChild && !mIsClient) {
        ConnectionLogCID1(tmpShortHeader.mConnectionID, "making one\n");
        MozQuic *child = Accept((struct sockaddr *)&peer, tmpShortHeader.mConnectionID,
                                tmpShortHeader.mPacketNumber);
        mChildren.emplace_back(child->mAlive);
        child->mConnectionState = SERVER_STATE_1RTT;
        if (mConnEventCB) {
          mConnEventCB(mClosure, MOZQUIC_EVENT_ACCEPT_NEW_CONNECTION, child);
        }
      } else {
        StatelessResetSend(tmpShortHeader.mConnectionID, (const sockaddr *) &peer);
      }
      rv = MOZQUIC_ERR_GENERAL;
      tmpSession = FindSession(&peer);
      if (!tmpSession) {
        continue;
      }
    }

    session = tmpSession->mAlive;
    ShortHeaderData shortHeader(this, pkt, pktSize, session->mNextRecvPacketNumber,
                                mLocalOmitCID,
                                mLocalOmitCID ? mConnectionID : 0);

    assert(shortHeader.mConnectionID == tmpShortHeader.mConnectionID);
    ConnectionLogCID5(shortHeader.mConnectionID,
                      "SHORTFORM PACKET[%d] pkt# %lX hdrsize=%d\n",
                      pktSize, shortHeader.mPacketNumber, shortHeader.mHeaderSize);
    rv = session->ProcessGeneral(pkt, pktSize,
                                 shortHeader.mHeaderSize, shortHeader.mPacketNumber, sendAck);
    if (rv == MOZQUIC_OK) {
      session->Acknowledge(shortHeader.mPacketNumber, keyPhase1Rtt);
    }

    if ((rv == MOZQUIC_OK) && sendAck) {
      rv = session->MaybeSendAck(true);
    }
  } while (rv == MOZQUIC_OK && !(*partialResult));

  return rv;
}

int
MozQuic::IO()
{
  uint32_t code;
  std::shared_ptr<MozQuic> deleteProtector(mAlive);
  ConnectionLog10("MozQuic::IO %p\n", this);

  bool partialResult = false;
  do {
    Intake(&partialResult);
    if (mStreamState) {
      mStreamState->Flush(false);
    }

    if (mIsClient) {
      switch (mConnectionState) {
      case CLIENT_STATE_1RTT:
      case CLIENT_STATE_0RTT:
        code = Client1RTT();
        if (code != MOZQUIC_OK) {
          return code;
        }
        break;
      case CLIENT_STATE_CONNECTED:
        break;
      case CLIENT_STATE_CLOSED:
      case SERVER_STATE_CLOSED:
        break;
      default:
        assert(false);
        // todo
      }
    } else {
      if ((mConnectionState == SERVER_STATE_1RTT) ||
          (mConnectionState == SERVER_STATE_0RTT)) {
        code = Server1RTT();
        if (code != MOZQUIC_OK) {
          return code;
        }
      }
      if (!mIsChild) {
        size_t len = mChildren.size();
        for (auto iter = mChildren.begin();
             len == mChildren.size() && iter != mChildren.end(); ++iter) {
          (*iter)->IO();
        }
      }
    }

  } while (partialResult);

  Timer::Tick();
  
  if ((mConnectionState == SERVER_STATE_1RTT || mConnectionState == SERVER_STATE_0RTT ||
       mConnectionState == CLIENT_STATE_1RTT || mConnectionState == CLIENT_STATE_0RTT) &&
      (mNextTransmitPacketNumber - mOriginalTransmitPacketNumber) > 14) {
    RaiseError(MOZQUIC_ERR_GENERAL, (char *)"TimedOut Client In Handshake");
    return MOZQUIC_ERR_GENERAL;
  }

  if (mConnEventCB) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_IO, this);
  }
  return MOZQUIC_OK;
}

uint32_t
MozQuic::Recv(unsigned char *pkt, uint32_t avail, uint32_t &outLen,
              struct sockaddr_in6 *peer)
{
  uint32_t code = MOZQUIC_OK;

  if (mAppHandlesSendRecv) {
    struct mozquic_eventdata_recv data;
    uint32_t written;

    data.pkt = pkt;
    data.avail = avail;
    data.written = &written;
    code = mConnEventCB(mClosure, MOZQUIC_EVENT_RECV, &data);
    outLen = written;
  } else {
    socklen_t sinlen = sizeof(*peer);
    ssize_t amt =
      recvfrom(mFD, pkt, avail, 0, (struct sockaddr *) peer, &sinlen);
    outLen = amt > 0 ? amt : 0;
    // todo errs
    code = MOZQUIC_OK;
  }
  if (code != MOZQUIC_OK) {
    return code;
  }

  if (outLen) {
    mIdleDeadline->Arm(mPeerIdleTimeout * 1000);
  }
  return MOZQUIC_OK;
}

void
MozQuic::RaiseError(uint32_t e, const char *fmt, ...)
{
  ConnectionLog1("RaiseError %u\n", e);

  va_list a;
  va_start(a, fmt);
  Log::sDoLog(Log::CONNECTION, 1, this, mConnectionID, fmt, a);
  va_end(a);
  
  if (mConnEventCB && (mIsClient || mIsChild)) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_ERROR, this);
  }
}

// this is called by the application when the application is handling
// the TLS stream (so that it can do more sophisticated handling
// of certs etc like gecko PSM does). The app is providing the
// client hello
void
MozQuic::HandshakeOutput(const unsigned char *buf, uint32_t datalen)
{
  assert(0);
  // dtls hack
}

void
MozQuic::HandshakeTParamOutput(const unsigned char *buf, uint32_t datalen)
{
  mRemoteTransportExtensionInfo.reset(new unsigned char[datalen]);
  mRemoteTransportExtensionInfoLen = datalen;
  memcpy(mRemoteTransportExtensionInfo.get(), buf, datalen);
}

// this is called by the application when the application is handling
// the TLS stream (so that it can do more sophisticated handling
// of certs etc like gecko PSM does). The app is providing the
// client hello and interpreting the server hello
uint32_t
MozQuic::HandshakeComplete(uint32_t code,
                           struct mozquic_handshake_info *keyInfo)
{
  if (!mAppHandlesSendRecv) {
    RaiseError(MOZQUIC_ERR_GENERAL, (char *)"not using handshaker api");
    return MOZQUIC_ERR_GENERAL;
  }
  assert (0);
  // dtls
}

uint32_t
MozQuic::ClientConnected()
{
  assert(mConnectionID == 0); // dtlsv1 hack

  ConnectionLog4("CLIENT_STATE_CONNECTED\n");
  assert((mConnectionState == CLIENT_STATE_1RTT) ||
         (mConnectionState == CLIENT_STATE_0RTT));
  mSendState->Connected();
  unsigned char *extensionInfo = nullptr;
  uint16_t extensionInfoLen = 0;
  uint32_t peerVersionList[256];
  uint16_t versionSize = sizeof(peerVersionList) / sizeof (peerVersionList[0]);

  if (!mAppHandlesSendRecv) {
    mNSSHelper->GetRemoteTransportExtensionInfo(extensionInfo, extensionInfoLen);
  } else {
    extensionInfo = mRemoteTransportExtensionInfo.get();
    extensionInfoLen = mRemoteTransportExtensionInfoLen;
  }

  uint32_t decodeResult;
  uint32_t errorCode = ERROR_NO_ERROR;
  if (!extensionInfoLen && mTolerateNoTransportParams) {
    ConnectionLog5("Decoding Server Transport Parameters: tolerated empty by config\n");
    decodeResult = MOZQUIC_OK;
  } else {
    assert(sizeof(mStatelessResetToken) == 16);
    uint32_t peerNegotiatedVersion;
    uint32_t peerMaxData;
    mStreamState->mPeerMaxStreamID[BIDI_STREAM] = 8;
    decodeResult =
      TransportExtension::
      DecodeServerTransportParameters(extensionInfo, extensionInfoLen,
                                      peerNegotiatedVersion,
                                      peerVersionList, versionSize,
                                      mStreamState->mPeerMaxStreamData,
                                      peerMaxData,
                                      mStreamState->mPeerMaxStreamID[BIDI_STREAM],
                                      mStreamState->mPeerMaxStreamID[UNI_STREAM],
                                      mPeerIdleTimeout,
                                      mPeerOmitCID, mMaxPacketConfig, mPeerAckDelayExponent,
                                      mStatelessResetToken, this);
    mStreamState->mPeerMaxData = peerMaxData;
    if (decodeResult != MOZQUIC_OK) {
      ConnectionLog1("Decoding Server Transport Parameters: failed\n");
      errorCode = ERROR_TRANSPORT_PARAMETER;
    } else {
      ConnectionLog5("Decoding Server Transport Parameters: passed\n");
    }
    mPeerIdleTimeout = std::min(mPeerIdleTimeout, (uint16_t)600); // 7.4.1
    mIdleDeadline->Arm(mPeerIdleTimeout * 1000);
    mRemoteTransportExtensionInfo = nullptr;
    mRemoteTransportExtensionInfoLen = 0;
    extensionInfo = nullptr;
    extensionInfoLen = 0;

    if (decodeResult == MOZQUIC_OK) {
      // disable for dtls
      decodeResult = (mVersion == peerNegotiatedVersion) ? MOZQUIC_OK : MOZQUIC_ERR_CRYPTO;
      decodeResult = MOZQUIC_OK;
      if (decodeResult != MOZQUIC_OK) {
        errorCode = ERROR_VERSION_NEGOTIATION;
        ConnectionLog1("Verify Server Transport Parameters: negotiated_version\n");
      }
    }

    // need to confirm version negotiation wasn't messed with
    if (0 && decodeResult == MOZQUIC_OK) { // disable for dtls
      // is mVersion in the peerVersionList?
      decodeResult = MOZQUIC_ERR_CRYPTO;
      for (int i = 0; i < versionSize; i++) {
        if (peerVersionList[i] == mVersion) {
          decodeResult = MOZQUIC_OK;
          break;
        }
      }
      if (decodeResult != MOZQUIC_OK) {
        errorCode = ERROR_VERSION_NEGOTIATION;
        ConnectionLog1("Verify Server Transport Parameters: version used failed\n");
      } else {
        ConnectionLog5("Verify Server Transport Parameters: version used passed\n");
      }
    }

    // if negotiation happened is the result correct?
    if (decodeResult == MOZQUIC_OK &&
        mVersion != mClientOriginalOfferedVersion) {
      decodeResult = MOZQUIC_ERR_CRYPTO;
      for (int i = 0; i < versionSize; i++) {
        if (VersionOK(peerVersionList[i])) {
          decodeResult = (peerVersionList[i] == mVersion) ? MOZQUIC_OK : MOZQUIC_ERR_CRYPTO;
          break;
        }
      }
      if (decodeResult != MOZQUIC_OK) {
        ConnectionLog1("Verify Server Transport Parameters: negotiation ok failed\n");
        errorCode = ERROR_VERSION_NEGOTIATION;
      } else {
        ConnectionLog5("Verify Server Transport Parameters: negotiation ok passed\n");
      }
    }
  }

  mConnectionState = CLIENT_STATE_CONNECTED;

  if (mEarlyDataState == EARLY_DATA_SENT) {
    if (mNSSHelper->IsEarlyDataAcceptedClient()) {
      mEarlyDataState = EARLY_DATA_ACCEPTED;
      mStreamState->DeleteDoneStreams();
    } else {
      mEarlyDataState = EARLY_DATA_IGNORED;
      mStreamState->Reset0RTTData();
    }
  }

  if (decodeResult != MOZQUIC_OK) {
    assert (errorCode != ERROR_NO_ERROR);
    MaybeSendAck();
    Shutdown(errorCode, "failed transport parameter verification");
    RaiseError(decodeResult, (char *) "failed to verify server transport parameters\n");
    return MOZQUIC_ERR_CRYPTO;
  }
  if (mConnEventCB) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_CONNECTED, this);
  }
  ReleaseProtectedPackets();
  return MaybeSendAck();
}

uint32_t
MozQuic::ServerConnected()
{
  assert (mIsChild && !mIsClient);
  assert(mConnectionID == 0); // dtlsv1 hack
  ConnectionLog4("SERVER_STATE_CONNECTED\n");
  assert((mConnectionState == SERVER_STATE_1RTT) ||
         (mConnectionState == SERVER_STATE_0RTT));
  mSendState->Connected();
  unsigned char *extensionInfo = nullptr;
  uint16_t extensionInfoLen = 0;
  uint32_t peerInitialVersion;
  mNSSHelper->GetRemoteTransportExtensionInfo(extensionInfo, extensionInfoLen);
  uint32_t decodeResult;
  uint32_t errorCode = ERROR_NO_ERROR;
  if (!extensionInfoLen && mTolerateNoTransportParams) {
    ConnectionLog6("Decoding Client Transport Parameters: tolerated empty by config\n");
    decodeResult = MOZQUIC_OK;
  } else {
    uint32_t peerMaxData;
    decodeResult =
      TransportExtension::
      DecodeClientTransportParameters(extensionInfo, extensionInfoLen,
                                      peerInitialVersion,
                                      mStreamState->mPeerMaxStreamData,
                                      peerMaxData,
                                      mStreamState->mPeerMaxStreamID[BIDI_STREAM],
                                      mStreamState->mPeerMaxStreamID[UNI_STREAM],
                                      mPeerIdleTimeout,
                                      mPeerOmitCID, mMaxPacketConfig, mPeerAckDelayExponent, this);
    mStreamState->mPeerMaxData = peerMaxData;
    ConnectionLog6(
            "decode client parameters: "
            "maxstreamdata %u "
            "maxdatabytes %u "
            "maxstreamidbidi %u "
            "maxstreamiduni %u "
            "idle %u "
            "omitCID %d "
            "maxpacket %u\n",
            mStreamState->mPeerMaxStreamData,
            mStreamState->mPeerMaxData,
            mStreamState->mPeerMaxStreamID[BIDI_STREAM],
            mStreamState->mPeerMaxStreamID[UNI_STREAM],
            mPeerIdleTimeout, mPeerOmitCID, mMaxPacketConfig);
    mPeerIdleTimeout = std::min(mPeerIdleTimeout, (uint16_t)600); // 7.4.1
    mIdleDeadline->Arm(mPeerIdleTimeout * 1000);

    Log::sDoLog(Log::CONNECTION, decodeResult == MOZQUIC_OK ? 5 : 1, this,
                "Decoding Client Transport Parameters: %s\n",
                decodeResult == MOZQUIC_OK ? "passed" : "failed");
    
    if (decodeResult != MOZQUIC_OK) {
      errorCode = ERROR_TRANSPORT_PARAMETER;
    } 
  }
  
  mConnectionState = SERVER_STATE_CONNECTED;
  if (decodeResult != MOZQUIC_OK) {
    assert(errorCode != ERROR_NO_ERROR);
    MaybeSendAck();
    Shutdown(errorCode, "failed transport parameter verification");
    RaiseError(decodeResult, (char *) "failed to verify client transport parameters\n");
    return MOZQUIC_ERR_CRYPTO;
  }
  
  if (mConnEventCB) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_CONNECTED, this);
  }
  ReleaseProtectedPackets();
  return MaybeSendAck();
}


uint32_t
MozQuic::BufferForLater(const unsigned char *pkt, uint32_t pktSize, uint32_t headerSize,
                        uint64_t packetNum)
{
  
  mBufferedProtectedPackets.emplace_back(pkt, pktSize, headerSize, packetNum);
  return MOZQUIC_ERR_DEFERRED;
}

uint32_t
MozQuic::ReleaseProtectedPackets()
{
  for (auto iter = mBufferedProtectedPackets.begin();
       iter != mBufferedProtectedPackets.end(); ++iter) {
    bool unused;
    ProcessGeneral(iter->mData.get(),
                   iter->mLen, iter->mHeaderSize, iter->mPacketNum, unused);
  }
  mBufferedProtectedPackets.clear();
  return MOZQUIC_OK;
}

uint32_t
MozQuic::Process0RTTProtectedPacket(const unsigned char *pkt, uint32_t pktSize, uint32_t headerSize,
                                    uint64_t packetNum, bool &sendAck)
{
  if ((mConnectionState == SERVER_STATE_SSR) ||
      (mConnectionState == SERVER_STATE_CLOSED) ||
      (mEarlyDataState == EARLY_DATA_IGNORED)) {
    ConnectionLog4("0RTT protected packet - ignore 0RTT packet due to state\n");
    return MOZQUIC_ERR_GENERAL;
  } else if (mEarlyDataState == EARLY_DATA_NOT_NEGOTIATED) {
    RaiseError(MOZQUIC_ERR_GENERAL, (char *)"A 0RTT encrypted packet, but 0RTT not negotiated.\n");
    return MOZQUIC_ERR_GENERAL;
  } else if (mConnectionState == SERVER_STATE_CONNECTED) {
    assert (mEarlyDataState == EARLY_DATA_ACCEPTED);
    if (packetNum > mEarlyDataLastPacketNumber) {
      ConnectionLog1("0RTT protected packet - 0RTT packet with a high packet number\n");
      RaiseError(MOZQUIC_ERR_GENERAL, (char *)"A 0RTT encrypted packet after handshake.\n");
      return MOZQUIC_ERR_GENERAL;
    }
  }
  return ProcessGeneral(pkt, pktSize, 17, packetNum, sendAck);
}

uint32_t
MozQuic::ProcessGeneral(const unsigned char *pkt, uint32_t pktSize, uint32_t headerSize,
                        uint64_t packetNum, bool &sendAck)
{
  assert(pktSize >= headerSize);
  assert(pktSize <= kMozQuicMSS);
  unsigned char out[kMozQuicMSS];

  if (mConnectionState == CLIENT_STATE_CLOSED ||
      mConnectionState == SERVER_STATE_CLOSED) {
    ConnectionLog4("processgeneral discarding %lX as closed\n", packetNum);
    return MOZQUIC_ERR_GENERAL;
  }

  if ((mConnectionState == CLIENT_STATE_1RTT ||
       mConnectionState == CLIENT_STATE_0RTT ||
       mConnectionState == SERVER_STATE_1RTT ||
       mConnectionState == SERVER_STATE_0RTT)) {
    ConnectionLog4("processgeneral buffering for later reassembly or nss handshake %lX\n", packetNum);
    sendAck = false;
    return BufferForLater(pkt, pktSize, headerSize, packetNum);
  }

  uint32_t written;
  uint32_t rv;

  rv = mNSSHelper->DecryptBlock(pkt, headerSize, pkt + headerSize,
                                pktSize - headerSize, packetNum, out,
                                kMozQuicMSS, written);

  ConnectionLog6("decrypt (pktnum=%lX) rv=%d sz=%d\n", packetNum, rv, written);
  if (rv != MOZQUIC_OK) {
    ConnectionLog1("decrypt failed\n");
    if (StatelessResetCheckForReceipt(pkt, pktSize)) {
      return MOZQUIC_OK;
    }
    return rv;
  }
  if (!mDecodedOK) {
    mDecodedOK = true;
//    StartPMTUD1(); // dtls hack because seqno is hard
  }
  if (mPingDeadline->Armed() && mConnEventCB) {
    mPingDeadline->Cancel();
    mConnEventCB(mClosure, MOZQUIC_EVENT_PING_OK, nullptr);
  }

  return ProcessGeneralDecoded(out, written, sendAck, false);
}

uint32_t
MozQuic::HandleConnCloseFrame(FrameHeaderData *, bool fromCleartext,
                              const unsigned char *, const unsigned char *,
                              uint32_t &/*_ptr*/)
{
  ConnectionLog5("RECVD CONN CLOSE handshake=%d\n", fromCleartext);
  mConnectionState = mIsClient ? CLIENT_STATE_CLOSED : SERVER_STATE_CLOSED;
  mStreamState->mUnAckedPackets.clear();
  if (mConnEventCB && !fromCleartext) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_CLOSE_CONNECTION, this);
  } else {
    ConnectionLog9("No Event callback\n");
  }
  return MOZQUIC_OK;
}

uint32_t
MozQuic::HandleApplicationCloseFrame(FrameHeaderData *, bool fromCleartext,
                                     const unsigned char *, const unsigned char *,
                                     uint32_t &/*_ptr*/)
{
  if (fromCleartext) {
    RaiseError(MOZQUIC_ERR_GENERAL, (char *) "app close frames not allowed in cleartext\n");
    return MOZQUIC_ERR_GENERAL;
  }
  ConnectionLog5("RECVD APP CLOSE\n");
  mConnectionState = mIsClient ? CLIENT_STATE_CLOSED : SERVER_STATE_CLOSED;
  if (mConnEventCB) {
    mConnEventCB(mClosure, MOZQUIC_EVENT_CLOSE_APPLICATION, this);
  } else {
    ConnectionLog9("No Event callback\n");
  }
  return MOZQUIC_OK;
}

uint32_t
MozQuic::ProcessGeneralDecoded(const unsigned char *pkt, uint32_t pktSize,
                               bool &sendAck, bool fromCleartext)
{
  // used by both client and server
  const unsigned char *endpkt = pkt + pktSize;
  uint32_t ptr = 0;
  uint32_t rv;
  assert(pktSize <= kMozQuicMSS);
  sendAck = false;

  // fromCleartext frames may only be ack, stream-0, and padding
  // and process_client_initial may not be ack

  while (ptr < pktSize) {
    FrameHeaderData result(pkt + ptr, pktSize - ptr, this, fromCleartext);
    if (result.mValid != MOZQUIC_OK) {
      return result.mValid;
    }
    ptr += result.mFrameLen;
    switch(result.mType) {

    case FRAME_TYPE_STREAM:
      sendAck = true;
      rv = mStreamState->HandleStreamFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv == MOZQUIC_ERR_ALREADY_FINISHED) {
        rv = MOZQUIC_OK;
      }
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_ACK:
      rv = HandleAckFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_PADDING:
      sendAck = true; // yes, padding is acked right away but pure acks are not
      break;

    case FRAME_TYPE_RST_STREAM:
      sendAck = true;
      rv = mStreamState->HandleResetStreamFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_CONN_CLOSE:
      sendAck = true;
      rv = HandleConnCloseFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_APPLICATION_CLOSE:
      sendAck = true;
      rv = HandleApplicationCloseFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_MAX_DATA:
      sendAck = true;
      rv = mStreamState->HandleMaxDataFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_MAX_STREAM_DATA:
      sendAck = true;
      rv = mStreamState->HandleMaxStreamDataFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_MAX_STREAM_ID:
      sendAck = true;
      rv = mStreamState->HandleMaxStreamIDFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;
      
    case FRAME_TYPE_PING:
      ConnectionLog5("recvd ping\n");
      sendAck = true;
      HandlePingFrame(&result, fromCleartext, pkt, endpkt, ptr);
      break;
            
    case FRAME_TYPE_PONG:
      ConnectionLog5("recvd pong\n");
      sendAck = true;
      HandlePongFrame(&result, fromCleartext, pkt, endpkt, ptr);
      break;

    case FRAME_TYPE_BLOCKED:
      sendAck = true;
      rv = mStreamState->HandleBlockedFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_STREAM_BLOCKED:
      sendAck = true;
      rv = mStreamState->HandleStreamBlockedFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_STREAM_ID_BLOCKED:
      sendAck = true;
      rv = mStreamState->HandleStreamIDBlockedFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    case FRAME_TYPE_STOP_SENDING:
      sendAck = true;
      rv = mStreamState->HandleStopSendingFrame(&result, fromCleartext, pkt, endpkt, ptr);
      if (rv != MOZQUIC_OK) {
        return rv;
      }
      break;

    default:
      sendAck = true;
      if (fromCleartext) {
        ConnectionLog1("unexpected frame type %d cleartext=%d\n", result.mType, fromCleartext);
        RaiseError(MOZQUIC_ERR_GENERAL, (char *) "unexpected frame type");
        return MOZQUIC_ERR_GENERAL;
      }
      break;
    }
    assert(pkt + ptr <= endpkt);
  }
  return MOZQUIC_OK;
}

void
MozQuic::GetPeerAddressHash(uint64_t cid, unsigned char *out, uint32_t *outLen)
{
  assert(mIsChild && !mIsClient);
  assert(*outLen >= 14 + sizeof(mParent->mValidationKey));

  *outLen = 0;
  unsigned char *ptr = out;

  if (mIPV6) {
    assert (mPeer.sin6_family == AF_INET6);
    memcpy(ptr, &mPeer.sin6_addr.s6_addr, 16);
    ptr += 16;
    memcpy(ptr, &mPeer.sin6_port, sizeof(in_port_t));
    ptr += sizeof(in_port_t);
  } else {
    const struct sockaddr_in *v4ptr = (const struct sockaddr_in *) &mPeer;
    assert (v4ptr->sin_family == AF_INET);
    memcpy(ptr, &(v4ptr->sin_addr.s_addr), sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    memcpy(ptr, &(v4ptr->sin_port), sizeof(in_port_t));
    ptr += sizeof(in_port_t);
  }

  // server chosen when generating retry, but client supplied when validating
  uint64_t connID = PR_htonll(cid);
  memcpy(ptr, &connID, sizeof (uint64_t));
  ptr += sizeof(uint64_t);
  memcpy(ptr, &mParent->mValidationKey, sizeof(mValidationKey));
  ptr += sizeof(mValidationKey);

  *outLen = ptr - out;
  return;
}

MozQuic *
MozQuic::Accept(const struct sockaddr *clientAddr, uint64_t aConnectionID, uint64_t aCIPacketNumber)
{
  MozQuic *child = new MozQuic(mHandleIO);
  child->mStreamState.reset(new StreamState(child, mAdvertiseStreamWindow, mAdvertiseConnectionWindow));
  child->mStreamState->InitIDs(1, 3, 4, 2, kMaxStreamIDClientDefaultBidi, kMaxStreamIDClientDefaultUni);
  child->mIsChild = true;
  child->mIsClient = false;
  child->mParent = this;
  child->mIPV6 = mIPV6;
  child->mConnectionState = SERVER_STATE_LISTEN;

  if (mIPV6) {
    memcpy(&child->mPeer, clientAddr, sizeof (struct sockaddr_in6));
    assert(child->mPeer.sin6_family == AF_INET6);
  } else {
    memcpy(&child->mPeer, clientAddr, sizeof (struct sockaddr_in));
    assert(((struct sockaddr_in *)(&child->mPeer))->sin_family == AF_INET);
  }
  
  child->mFD = mFD;
  child->mClientInitialPacketNumber = aCIPacketNumber;

  child->SetInitialPacketNumber();

  child->mNSSHelper.reset(new NSSHelper(child, mTolerateBadALPN, mOriginName.get()));
  child->mVersion = mVersion;
  child->mDropRate = mDropRate;
  child->mTimestampConnBegin = Timestamp();
  child->mOriginalConnectionID = aConnectionID;
  child->mAppHandlesSendRecv = mAppHandlesSendRecv;
  child->mAppHandlesLogging = mAppHandlesLogging;
  mConnectionHash.insert( { SerializeSockaddr(clientAddr), child });

  // the struct can hold the unique ptr to the timer
  // the hash has to be a hash of unique pointers to structs

  std::unique_ptr<InitialClientPacketInfo> t(new InitialClientPacketInfo());
  t->mServerConnectionID = child->mConnectionID;
  t->mHashKey = aConnectionID;
  t->mTimestamp = Timestamp();
  
  return child;
}

bool
MozQuic::VersionOK(uint32_t proposed)
{
  if (proposed == kMozQuicVersion1 ||
      proposed == kMozQuicIetfID9) {
    return true;
  }
  return false;
}

uint32_t
MozQuic::StartNewStream(StreamPair **outStream, bool uni, bool no_replay,
                        const void *data, uint32_t amount, bool fin)
{
  if (mStreamState) {
    return mStreamState->StartNewStream(outStream, (uni) ? UNI_STREAM : BIDI_STREAM, no_replay, data, amount, fin);
  }
  return MOZQUIC_ERR_GENERAL;
}

void
MozQuic::MaybeDeleteStream(StreamPair *sp)
{
  if (sp) {
    mStreamState->MaybeDeleteStream(sp->mStreamID);
  }
}

uint64_t
MozQuic::Timestamp()
{
  // ms since epoch
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

int32_t
MozQuic::NSSInput(void *buf, int32_t amount)
{
  if (mBufferedProtectedPackets.empty()) {
    PR_SetError(PR_WOULD_BLOCK_ERROR, 0);
    return -1;
  }

  auto pkt = mBufferedProtectedPackets.begin();
  memcpy(buf, pkt->mData.get(), pkt->mLen);
  int32_t rv = pkt->mLen;
  mBufferedProtectedPackets.erase(pkt);
  return rv;
}

int32_t
MozQuic::NSSOutput(const void *iBuf, int32_t amount)
{
  if (amount < 11) {
    assert(false); // this can't be dtls
    return 0;
  }

  const unsigned char *buf = (const unsigned char *)iBuf;
  uint16_t epoch;
  uint64_t seqno;
  memcpy (&epoch, buf + 3, 2);
  epoch = ntohs(epoch);
  seqno = 0;
  memcpy (((unsigned char *)&seqno) + 2, buf + 5, 6);
  seqno = PR_ntohll(seqno);
  ConnectionLog8("Output DTLS from NSS epoch %u seqno %lx\n", epoch, seqno);

  RealTransmit(buf, amount, nullptr, false);
  return  amount;
}

}

