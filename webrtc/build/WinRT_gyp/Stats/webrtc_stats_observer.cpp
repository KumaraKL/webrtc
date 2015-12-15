
// Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.

#include <collection.h>
#include <string>

#include "webrtc/build/WinRT_gyp/Stats/webrtc_stats_observer.h"
#include "webrtc/base/thread.h"
#include "webrtc/build/WinRT_gyp/Api/RTCStatsReport.h"
#include "webrtc/build/WinRT_gyp/Api/Marshalling.h"

// generated by message compiler
#include "webrtc/build/WinRT_gyp/Stats/etw_providers.h"
#include "webrtc/system_wrappers/include/critical_section_wrapper.h"

using Platform::Collections::Vector;

namespace webrtc {

// The time interval of requesting statistics
const int WebRTCStatsObserver::kInterval = 1000;
enum {
  MSG_POLL_STATS,
};

ConnectionHealthStats::ConnectionHealthStats() : timestamp(0),
  received_bytes(0), received_kbps(0), sent_bytes(0), sent_kbps(0), rtt(0) {
}

void ConnectionHealthStats::Reset() {
  timestamp = 0;
  received_bytes = 0;
  received_kbps = 0;
  sent_bytes = 0;
  sent_kbps = 0;
  rtt = 0;
  local_candidate_type = "";
  remote_candidate_type = "";
}

WebRTCStatsObserver::WebRTCStatsObserver(
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pci) :
  pci_(pci), status_(kStopped),
  crit_sect_(CriticalSectionWrapper::CreateCriticalSection()),
  webrtc_stats_observer_winrt_(NULL), etw_stats_enabled_(false),
  rtc_stats_enabled_(false), conn_health_stats_enabled_(false) {
  EventRegisterWebRTCInternals();
}

WebRTCStatsObserver::~WebRTCStatsObserver() {
  EventUnregisterWebRTCInternals();
  // Will trigger Stop()
  ToggleETWStats(false);
  ToggleConnectionHealthStats(false);
  ToggleRTCStats(false);
}

void WebRTCStatsObserver::Start() {
  bool performStart = false;
  {
    CriticalSectionScoped lock(crit_sect_.get());
    if (status_ == kStopped) {
      performStart = true;
      LOG(LS_INFO) << "WebRTCStatsObserver starting";
    }
    status_ = kStarted;
  }

  if (performStart) {
    PollStats();
  }
}

void WebRTCStatsObserver::Stop() {
  CriticalSectionScoped lock(crit_sect_.get());
  if (status_ == kStarted) {
    status_ = kStopping;
    LOG(LS_INFO) << "WebRTCStatsObserver stopping";
  }
}

void WebRTCStatsObserver::ToggleETWStats(bool enable) {
  LOG(LS_INFO) << "WebRTCStatsObserver "
    << (enable ? "enabling" : "disabling")
    << " ETW stats";
  etw_stats_enabled_ = enable;
  EvaluatePollNecessity();
}

void WebRTCStatsObserver::ToggleConnectionHealthStats(
  WebRTCStatsObserverWinRT* observer) {
  if (observer) {
    webrtc_stats_observer_winrt_ = observer;
    LOG(LS_INFO) << "WebRTCStatsObserver enabling connection health stats";
    conn_health_stats_enabled_ = true;
  } else {
    LOG(LS_INFO) << "WebRTCStatsObserver disabling connection health stats";
    conn_health_stats_enabled_ = false;
  }
  EvaluatePollNecessity();
}

void WebRTCStatsObserver::ToggleRTCStats(WebRTCStatsObserverWinRT* observer) {
  if (observer) {
    webrtc_stats_observer_winrt_ = observer;
    LOG(LS_INFO) << "WebRTCStatsObserver enabling rtc stats";
    rtc_stats_enabled_ = true;
  } else {
    LOG(LS_INFO) << "WebRTCStatsObserver disabling rtc stats";
    rtc_stats_enabled_ = false;
  }
  EvaluatePollNecessity();
}

void WebRTCStatsObserver::OnComplete(const StatsReports& reports) {
  webrtc_winrt_api::RTCStatsReports rtcStatsReports =
                ref new Vector<webrtc_winrt_api::RTCStatsReport^>();
  for (auto report : reports) {
    std::string sgn = report->id()->ToString();
    auto stat_group_name = sgn.c_str();
    auto stat_type = report->id()->type();
    auto timestamp = report->timestamp();

    if (etw_stats_enabled_) {
      bool sendToEtwPlugin = false;
      if (stat_type == StatsReport::kStatsReportTypeSession ||
        stat_type == StatsReport::kStatsReportTypeTrack) {
        sendToEtwPlugin = true;
      } else if (stat_type == StatsReport::kStatsReportTypeSsrc) {
        const StatsReport::Value* v = report->FindValue(
          StatsReport::kStatsValueNameTrackId);
        if (v) {
          const std::string& id = v->string_val();
          auto ls = pci_->local_streams();
          auto rs = pci_->remote_streams();
          if (ls->FindAudioTrack(id) || ls->FindVideoTrack(id) ||
              rs->FindAudioTrack(id) || rs->FindVideoTrack(id)) {
            sendToEtwPlugin = true;
          }
        }
      }

      if (sendToEtwPlugin) {
        for (auto value : report->values()) {
          auto stat_name = value.second->display_name();
          switch (value.second->type()) {
          case StatsReport::Value::kInt:
            EventWriteStatsReportInt32(stat_group_name, timestamp, stat_name,
              value.second->int_val());
            break;
          case StatsReport::Value::kInt64:
            EventWriteStatsReportInt64(stat_group_name, timestamp, stat_name,
              value.second->int64_val());
            break;
          case StatsReport::Value::kFloat:
            EventWriteStatsReportFloat(stat_group_name, timestamp, stat_name,
              value.second->float_val());
            break;
          case StatsReport::Value::kBool:
            EventWriteStatsReportBool(stat_group_name, timestamp, stat_name,
              value.second->bool_val());
            break;
          case StatsReport::Value::kStaticString:
            EventWriteStatsReportString(stat_group_name, timestamp, stat_name,
              value.second->static_string_val());
            break;
          case StatsReport::Value::kString:
            EventWriteStatsReportString(stat_group_name, timestamp, stat_name,
              value.second->string_val().c_str());
            break;
          default:
            break;
          }
        }
        int64 memUsage = webrtc_winrt_api::WebRTC::GetMemUsage();
        double cpuUsage = webrtc_winrt_api::WebRTC::GetCPUUsage();

        //(ToDo: Ling)
        // Temporary put CPU/Memory usage under video group for now.
        // will need to sync with bakshi for where is the proper place to put them.
        /**
        EventWriteStatsReportInt64("System_Resource", timestamp, "MemUsage",
        memUsage);


        EventWriteStatsReportFloat("System_Resource", timestamp, "CPUUsage",
        cpuUsage);

         */

        EventWriteStatsReportInt64(stat_group_name, timestamp, "MemUsage",
          memUsage);


        EventWriteStatsReportFloat(stat_group_name, timestamp, "CPUUsage",
          cpuUsage);

      }
    }
    if (conn_health_stats_enabled_ && webrtc_stats_observer_winrt_) {
      if (stat_type == StatsReport::kStatsReportTypeCandidatePair) {
        for (auto value : report->values()) {
          if (value.first == StatsReport::kStatsValueNameActiveConnection) {
            if (!value.second->bool_val())
              break;
          } else if (value.first == StatsReport::kStatsValueNameBytesReceived) {
            conn_health_stats_.timestamp = timestamp;
            conn_health_stats_.received_bytes = value.second->int64_val();
          } else if (value.first == StatsReport::kStatsValueNameBytesSent) {
            conn_health_stats_.timestamp = timestamp;
            conn_health_stats_.sent_bytes = value.second->int64_val();
          } else if (value.first == StatsReport::kStatsValueNameRtt) {
            conn_health_stats_.rtt = value.second->int64_val();
          } else if (value.first ==
                     StatsReport::kStatsValueNameRemoteCandidateType) {
            conn_health_stats_.remote_candidate_type =
              value.second->string_val();
          } else if (value.first ==
                     StatsReport::kStatsValueNameLocalCandidateType) {
            conn_health_stats_.local_candidate_type =
              value.second->string_val();
          }
        }
      }
    }

    if (rtc_stats_enabled_ && webrtc_stats_observer_winrt_) {
      webrtc_winrt_api::RTCStatsReport^ rtcReport;
      webrtc_winrt_api_internal::ToCx(report, &rtcReport);
      rtcStatsReports->Append(rtcReport);
    }
  }

  if (rtcStatsReports->Size != 0) {
    webrtc_stats_observer_winrt_->OnRTCStatsReportsReady(rtcStatsReports);
  }
  if (conn_health_stats_enabled_ && webrtc_stats_observer_winrt_) {
    if (conn_health_stats_prev.timestamp != 0 &&
        conn_health_stats_.timestamp != conn_health_stats_prev.timestamp) {
      int64 time_elapsed_ms = static_cast<int64>(conn_health_stats_.timestamp -
                                      conn_health_stats_prev.timestamp);

      if (conn_health_stats_prev.sent_bytes < conn_health_stats_.sent_bytes) {
        conn_health_stats_.sent_kbps = 8 * 1000 *
          (conn_health_stats_.sent_bytes - conn_health_stats_prev.sent_bytes) /
          time_elapsed_ms / 1024;
      }
      if (conn_health_stats_prev.received_bytes <
        conn_health_stats_.received_bytes) {
        conn_health_stats_.received_kbps = 8 * 1000 *
          (conn_health_stats_.received_bytes -
           conn_health_stats_prev.received_bytes) /
          time_elapsed_ms / 1024;
      }
    }
    webrtc_stats_observer_winrt_->OnConnectionHealthStats(conn_health_stats_);
  }
}

void WebRTCStatsObserver::OnMessage(rtc::Message* msg) {
  switch (msg->message_id) {
  case MSG_POLL_STATS:
    PollStats();
    break;
  default:
    break;
  }
}

void WebRTCStatsObserver::PollStats() {
  if (!pci_.get()) {
    return;
  }

  GetAllStats();
  /*auto lss = pci_->local_streams();
  GetStreamCollectionStats(lss);*/

  {
    CriticalSectionScoped lock(crit_sect_.get());
    if (status_ != kStarted) {
      LOG(LS_INFO) << "WebRTCStatsObserver stopped";
      status_ = kStopped;
      return;
    }
    rtc::Thread::Current()->PostDelayed(kInterval, this, MSG_POLL_STATS);
  }
}
void WebRTCStatsObserver::EvaluatePollNecessity() {
  if (etw_stats_enabled_ || webrtc_stats_observer_winrt_ ||
    rtc_stats_enabled_ || conn_health_stats_enabled_) {
    Start();
  } else {
    Stop();
  }
}

void WebRTCStatsObserver::GetStreamCollectionStats(
  rtc::scoped_refptr<StreamCollectionInterface> streams) {
  auto ss_count = streams->count();
  for (size_t i = 0; i < ss_count; ++i) {
    auto audio_tracks = streams->at(i)->GetAudioTracks();
    auto video_tracks = streams->at(i)->GetVideoTracks();

    for (auto audio_track : audio_tracks) {
      pci_->GetStats(this, audio_track,
        PeerConnectionInterface::kStatsOutputLevelDebug);
    }

    for (auto video_track : video_tracks) {
      pci_->GetStats(this, video_track,
        PeerConnectionInterface::kStatsOutputLevelDebug);
    }
  }
}

void WebRTCStatsObserver::GetAllStats() {
  if (conn_health_stats_enabled_) {
    conn_health_stats_prev = conn_health_stats_;
    conn_health_stats_.Reset();
  }
  pci_->GetStats(this, NULL, PeerConnectionInterface::kStatsOutputLevelDebug);
}
}  //  namespace webrtc
