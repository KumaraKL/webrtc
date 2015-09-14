/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef WEBRTC_TEST_CHANNEL_TRANSPORT_UDP_SOCKET_MANAGER_WINRT_H_
#define WEBRTC_TEST_CHANNEL_TRANSPORT_UDP_SOCKET_MANAGER_WINRT_H_

#include <winsock2.h>

#include <list>
#include <map>

#include "webrtc/system_wrappers/interface/critical_section_wrapper.h"
#include "webrtc/system_wrappers/interface/thread_wrapper.h"
#include "webrtc/test/channel_transport/udp_socket_manager_wrapper.h"
#include "webrtc/test/channel_transport/udp_socket_wrapper.h"

namespace webrtc {

class ConditionVariableWrapper;

namespace test {

class UdpSocketWinRT;
class UdpSocketManagerWinRTImpl;
#define MAX_NUMBER_OF_SOCKET_MANAGERS_LINUX 8

class UdpSocketManagerWinRT : public UdpSocketManager {
 public:
    UdpSocketManagerWinRT();
    virtual ~UdpSocketManagerWinRT();

    bool Init(int32_t id, uint8_t& numOfWorkThreads) override;

    bool Start() override;
    bool Stop() override;

    bool AddSocket(UdpSocketWrapper* s) override;
    bool RemoveSocket(UdpSocketWrapper* s) override;

 private:
    int32_t _id;
    CriticalSectionWrapper* _critSect;
    uint8_t _numberOfSocketMgr;
    uint8_t _incSocketMgrNextTime;
    uint8_t _nextSocketMgrToAssign;
    UdpSocketManagerWinRTImpl* _socketMgr[MAX_NUMBER_OF_SOCKET_MANAGERS_LINUX];
};

class UdpSocketManagerWinRTImpl {
 public:
    UdpSocketManagerWinRTImpl();
    virtual ~UdpSocketManagerWinRTImpl();

    virtual bool Start();
    virtual bool Stop();

    virtual bool AddSocket(UdpSocketWrapper* s);
    virtual bool RemoveSocket(UdpSocketWrapper* s);

 protected:
    static bool Run(void* obj);
    bool Process();
    void UpdateSocketMap();

 private:
    typedef std::list<UdpSocketWrapper*> SocketList;
    typedef std::list<SOCKET> FdList;
    rtc::scoped_ptr<ThreadWrapper> _thread;
    CriticalSectionWrapper* _critSectList;

    fd_set _readFds;

    std::map<SOCKET, UdpSocketWinRT*> _socketMap;
    SocketList _addList;
    FdList _removeList;
};

}  // namespace test
}  // namespace webrtc

#endif  // WEBRTC_TEST_CHANNEL_TRANSPORT_UDP_SOCKET_MANAGER_WINRT_H_
