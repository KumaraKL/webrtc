/*
*  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
*
*  Use of this source code is governed by a BSD-style license
*  that can be found in the LICENSE file in the root of the source
*  tree. An additional intellectual property rights grant can be found
*  in the file PATENTS.  All contributing project authors may
*  be found in the AUTHORS file in the root of the source tree.
*/

#include "webrtc/modules/video_capture/windows/video_capture_winrt.h"

#include <ppltasks.h>

#include <string>

#include "webrtc/system_wrappers/interface/logging.h"
#include "webrtc/modules/video_capture/windows/video_capture_sink_winrt.h"

using Microsoft::WRL::ComPtr;
using Windows::Devices::Enumeration::DeviceClass;
using Windows::Devices::Enumeration::DeviceInformation;
using Windows::Devices::Enumeration::DeviceInformationCollection;
using Windows::Media::Capture::MediaCapture;
using Windows::Media::Capture::MediaCaptureFailedEventArgs;
using Windows::Media::Capture::MediaCaptureFailedEventHandler;
using Windows::Media::Capture::MediaCaptureInitializationSettings;
using Windows::Media::Capture::MediaStreamType;
using Windows::Media::IMediaExtension;
using Windows::Media::MediaProperties::IVideoEncodingProperties;
using Windows::Media::MediaProperties::MediaEncodingProfile;
using Windows::Media::MediaProperties::MediaEncodingSubtypes;
using Windows::Media::MediaProperties::VideoEncodingProperties;

extern Windows::UI::Core::CoreDispatcher^ g_windowDispatcher;

namespace webrtc {
namespace videocapturemodule {

ref class CaptureDevice sealed {
 internal:
  CaptureDevice(CaptureDeviceListener* capture_device_listener);

  void Initialize(Platform::String^ device_id);

  void CleanupSink();

  void Cleanup();

  void StartCapture(MediaEncodingProfile^ media_encoding_profile,
                    IVideoEncodingProperties^ video_encoding_properties);

  void StopCapture();

  bool CaptureStarted() { return capture_started_; }

  VideoCaptureCapability GetFrameInfo() { return frame_info_; }

  void OnCaptureFailed(MediaCapture^ sender,
                       MediaCaptureFailedEventArgs^ error_event_args);

  void OnMediaSample(Object^ sender, MediaSampleEventArgs^ args);

  property Platform::Agile<Windows::Media::Capture::MediaCapture> MediaCapture {
    Platform::Agile<Windows::Media::Capture::MediaCapture> get();
  }

private:
  Platform::Agile<Windows::Media::Capture::MediaCapture> media_capture_;
  VideoCaptureMediaSinkProxyWinRT^ media_sink_;
  Windows::Foundation::EventRegistrationToken
    media_capture_failed_event_registration_token_;
  Windows::Foundation::EventRegistrationToken
    media_sink_video_sample_event_registration_token_;

  CaptureDeviceListener* capture_device_listener_;

  bool capture_started_;
  VideoCaptureCapability frame_info_;
};

CaptureDevice::CaptureDevice(
  CaptureDeviceListener* capture_device_listener)
  : media_capture_(nullptr),
    media_sink_(nullptr),
    capture_device_listener_(capture_device_listener),
    capture_started_(false) {
}

void CaptureDevice::Initialize(Platform::String^ device_id) {
  media_capture_ =
    MediaCaptureDevicesWinRT::Instance()->GetMediaCapture(device_id);
  media_capture_failed_event_registration_token_ =
    media_capture_->Failed +=
      ref new MediaCaptureFailedEventHandler(this,
                                             &CaptureDevice::OnCaptureFailed);
}

void CaptureDevice::CleanupSink() {
  if (media_sink_) {
    media_sink_->MediaSampleEvent -=
      media_sink_video_sample_event_registration_token_;
    delete media_sink_;
    media_sink_ = nullptr;
    capture_started_ = false;
  }
}

Platform::Agile<Windows::Media::Capture::MediaCapture>
CaptureDevice::MediaCapture::get() {
  return media_capture_;
}

void CaptureDevice::Cleanup() {
  Windows::Media::Capture::MediaCapture^ media_capture = media_capture_.Get();
  if (media_capture == nullptr) {
    return;
  }

  if (capture_started_) {
    Concurrency::create_task(
      media_capture->StopRecordAsync()).then([this](Concurrency::task<void>&) {
    }).wait();
  }
  media_capture->Failed -= media_capture_failed_event_registration_token_;
  CleanupSink();
}

void CaptureDevice::StartCapture(
  MediaEncodingProfile^ media_encoding_profile,
  IVideoEncodingProperties^ video_encoding_properties) {
  if (capture_started_) {
    throw ref new Platform::Exception(
      __HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
  }

  CleanupSink();

  frame_info_.width = media_encoding_profile->Video->Width;
  frame_info_.height = media_encoding_profile->Video->Height;
  frame_info_.maxFPS =
    static_cast<int>(
      static_cast<float>(
        media_encoding_profile->Video->FrameRate->Numerator) /
      static_cast<float>(
        media_encoding_profile->Video->FrameRate->Denominator));
  if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Yv12->Data()) == 0)
    frame_info_.rawType = kVideoYV12;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Yuy2->Data()) == 0)
    frame_info_.rawType = kVideoYUY2;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Iyuv->Data()) == 0)
    frame_info_.rawType = kVideoIYUV;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Rgb24->Data()) == 0)
    frame_info_.rawType = kVideoRGB24;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Rgb32->Data()) == 0)
    frame_info_.rawType = kVideoARGB;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Mjpg->Data()) == 0)
    frame_info_.rawType = kVideoMJPEG;
  else if (_wcsicmp(media_encoding_profile->Video->Subtype->Data(),
    MediaEncodingSubtypes::Nv12->Data()) == 0)
    frame_info_.rawType = kVideoNV12;
  else
    frame_info_.rawType = kVideoUnknown;

  media_sink_ = ref new VideoCaptureMediaSinkProxyWinRT();
  media_sink_video_sample_event_registration_token_ =
    media_sink_->MediaSampleEvent +=
      ref new Windows::Foundation::EventHandler<MediaSampleEventArgs^>
        (this, &CaptureDevice::OnMediaSample);

  Concurrency::create_task(
    media_sink_->InitializeAsync(media_encoding_profile->Video)).
      then([this,
            media_encoding_profile,
            video_encoding_properties](IMediaExtension^ media_extension) {
    return Concurrency::create_task(
      media_capture_->VideoDeviceController->SetMediaStreamPropertiesAsync(
        MediaStreamType::VideoRecord,
        video_encoding_properties)).
          then([this,
                media_encoding_profile,
                media_extension](Concurrency::task<void> async_info) {
      return Concurrency::create_task(media_capture_->
        StartRecordToCustomSinkAsync(media_encoding_profile,
                                     media_extension)).
          then([this](Concurrency::task<void> async_info) {
        try {
          async_info.get();
          capture_started_ = true;
        } catch (Platform::Exception^ e) {
          CleanupSink();
          throw;
        }
      });
    });
  }).wait();
}

void CaptureDevice::StopCapture() {
  if (!capture_started_) {
    throw ref new Platform::Exception(
      __HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
  }

  Concurrency::create_task(
    media_capture_.Get()->StopRecordAsync()).
      then([this](Concurrency::task<void>& asyncInfo) {
    try {
      asyncInfo.get();
      CleanupSink();
    } catch (Platform::Exception^ e) {
      CleanupSink();
      throw;
    }
  }).wait();
}

void CaptureDevice::OnCaptureFailed(
  Windows::Media::Capture::MediaCapture^ sender,
  MediaCaptureFailedEventArgs^ error_event_args) {
  if (capture_device_listener_) {
    capture_device_listener_->OnCaptureDeviceFailed(
      error_event_args->Code,
      error_event_args->Message);
  }
}

void CaptureDevice::OnMediaSample(Object^ sender, MediaSampleEventArgs^ args) {
  if (capture_device_listener_) {
    Microsoft::WRL::ComPtr<IMFSample> spMediaSample = args->GetMediaSample();
    ComPtr<IMFMediaBuffer> spMediaBuffer;
    HRESULT hr = spMediaSample->GetBufferByIndex(0, &spMediaBuffer);
    LONGLONG hnsSampleTime = 0;
    BYTE* pbBuffer = NULL;
    DWORD cbMaxLength = 0;
    DWORD cbCurrentLength = 0;
    if (SUCCEEDED(hr)) {
      hr = spMediaSample->GetSampleTime(&hnsSampleTime);
    }
    if (SUCCEEDED(hr)) {
      hr = spMediaBuffer->Lock(&pbBuffer, &cbMaxLength, &cbCurrentLength);
    }
    if (SUCCEEDED(hr)) {
      uint8_t* video_frame;
      size_t video_frame_length;
      int64_t capture_time;
      video_frame = pbBuffer;
      video_frame_length = cbCurrentLength;
      // conversion from 100-nanosecond to millisecond units
      capture_time = hnsSampleTime / 10000;
      LOG(LS_VERBOSE) <<
        "Video Capture - Media sample received - video frame length: " <<
        video_frame_length <<", capture time : " << capture_time;
      capture_device_listener_->OnIncomingFrame(video_frame,
                                                video_frame_length,
                                                frame_info_);
    }
    if (SUCCEEDED(hr)) {
      hr = spMediaBuffer->Unlock();
    } else {
      LOG(LS_ERROR) << "Failed to send media sample. " << hr;
    }
  }
}

VideoCaptureWinRT::VideoCaptureWinRT(const int32_t id)
  : VideoCaptureImpl(id),
    device_(nullptr) {
}

VideoCaptureWinRT::~VideoCaptureWinRT() {
  if (device_ != nullptr)
    device_->Cleanup();
}

int32_t VideoCaptureWinRT::Init(const int32_t id,
                                const char* device_unique_id) {
  CriticalSectionScoped cs(&_apiCs);
  const int32_t device_unique_id_length = strlen(device_unique_id);
  if (device_unique_id_length > kVideoCaptureUniqueNameLength) {
    LOG(LS_ERROR) << "Device name too long";
    return -1;
  }

  LOG(LS_INFO) << "Init called for device " << device_unique_id;

  device_id_ = nullptr;

  Concurrency::create_task(
    DeviceInformation::FindAllAsync(DeviceClass::VideoCapture)).
      then([this,
           device_unique_id,
           device_unique_id_length](
        Concurrency::task<DeviceInformationCollection^> find_task) {
    try {
      DeviceInformationCollection^ dev_info_collection = find_task.get();
      if (dev_info_collection == nullptr || dev_info_collection->Size == 0) {
        LOG(LS_ERROR) << "There is no capture device installed on the system";
      }
      for (unsigned int i = 0; i < dev_info_collection->Size; i++) {
        auto dev_info = dev_info_collection->GetAt(i);
        Platform::String^ deviceUniqueId = dev_info->Id;
        char current_device_unique_id_utf8[256];
        current_device_unique_id_utf8[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, deviceUniqueId->Begin(), -1,
          current_device_unique_id_utf8,
          sizeof(current_device_unique_id_utf8), NULL,
          NULL);
        if (strncmp(current_device_unique_id_utf8,
          (const char*)device_unique_id,
          device_unique_id_length) == 0) {
          device_id_ = dev_info->Id;
          break;
        }
      }
    } catch (Platform::Exception^ e) {
      int message_size = WideCharToMultiByte(
        CP_UTF8, 0, e->Message->Data(),
        static_cast<int>(wcslen(e->Message->Data())),
        NULL, 0, NULL, NULL);
      std::string message(message_size, 0);
      WideCharToMultiByte(
        CP_UTF8, 0, e->Message->Data(),
        static_cast<int>(wcslen(e->Message->Data())),
        &message[0], message_size, NULL, NULL);
      LOG(LS_ERROR) <<
        "Failed to retrieve device info collection. " << message.c_str();
    }
  }).wait();

  if (device_id_ == nullptr) {
    LOG(LS_ERROR) << "No video capture device found";
    return -1;
  }

  device_ = ref new CaptureDevice(this);

  device_->Initialize(device_id_);

  return 0;
}

int32_t VideoCaptureWinRT::StartCapture(
  const VideoCaptureCapability& capability) {
  CriticalSectionScoped cs(&_apiCs);
  Platform::String^ subtype;
  switch (capability.rawType) {
  case kVideoYV12:
    subtype = MediaEncodingSubtypes::Yv12;
    break;
  case kVideoYUY2:
    subtype = MediaEncodingSubtypes::Yuy2;
    break;
  case kVideoI420:
  case kVideoIYUV:
    subtype = MediaEncodingSubtypes::Iyuv;
    break;
  case kVideoRGB24:
    subtype = MediaEncodingSubtypes::Rgb24;
    break;
  case kVideoARGB:
    subtype = MediaEncodingSubtypes::Rgb24;
    break;
  case kVideoMJPEG:
    subtype = MediaEncodingSubtypes::Mjpg;
    break;
  case kVideoNV12:
    subtype = MediaEncodingSubtypes::Nv12;
    break;
  default:
    LOG(LS_ERROR) <<
      "The specified raw video format is not supported on this plaform.";
    return -1;
  }

  MediaEncodingProfile^ media_encoding_profile =
    ref new MediaEncodingProfile();
  media_encoding_profile->Audio = nullptr;
  media_encoding_profile->Container = nullptr;
  media_encoding_profile->Video =
    VideoEncodingProperties::CreateUncompressed(
      subtype, capability.width, capability.height);
  media_encoding_profile->Video->FrameRate->Numerator = capability.maxFPS;
  media_encoding_profile->Video->FrameRate->Denominator = 1;

  IVideoEncodingProperties^ video_encoding_properties;
  int min_width_diff = INT_MAX;
  int min_height_diff = INT_MAX;
  int min_fps_diff = INT_MAX;
  auto mediaCapture =
    MediaCaptureDevicesWinRT::Instance()->GetMediaCapture(device_id_);
  auto streamProperties =
    mediaCapture->VideoDeviceController->GetAvailableMediaStreamProperties(
      MediaStreamType::VideoRecord);
  for (unsigned int i = 0; i < streamProperties->Size; i++) {
    IVideoEncodingProperties^ prop =
      static_cast<IVideoEncodingProperties^>(streamProperties->GetAt(i));

    if (_wcsicmp(prop->Subtype->Data(), subtype->Data()) != 0)
      continue;

    int width_diff = abs(static_cast<int>(prop->Width - capability.width));
    int height_diff = abs(static_cast<int>(prop->Height - capability.height));
    int prop_fps = static_cast<int>(
      (static_cast<float>(prop->FrameRate->Numerator) /
      static_cast<float>(prop->FrameRate->Denominator)));
    int fps_diff = abs(static_cast<int>(prop_fps - capability.maxFPS));

    if (width_diff < min_width_diff) {
      video_encoding_properties = prop;
      min_width_diff = width_diff;
      min_height_diff = height_diff;
      min_fps_diff = fps_diff;
    } else if (width_diff == min_width_diff) {
      if (height_diff < min_height_diff) {
        video_encoding_properties = prop;
        min_height_diff = height_diff;
        min_fps_diff = fps_diff;
      } else if (height_diff == min_height_diff) {
        if (fps_diff < min_fps_diff) {
          video_encoding_properties = prop;
          min_fps_diff = fps_diff;
        }
      }
    }
  }

  try {
    device_->StartCapture(media_encoding_profile, video_encoding_properties);
  } catch (Platform::Exception^ e) {
    int message_size = WideCharToMultiByte(
      CP_UTF8, 0, e->Message->Data(),
      static_cast<int>(wcslen(e->Message->Data())),
      NULL, 0, NULL, NULL);
    std::string message(message_size, 0);
    WideCharToMultiByte(
      CP_UTF8, 0, e->Message->Data(),
      static_cast<int>(wcslen(e->Message->Data())),
      &message[0], message_size, NULL, NULL);
    LOG(LS_ERROR) << "Failed to start capture. " << message.c_str();
    return -1;
  }

  return 0;
}

int32_t VideoCaptureWinRT::StopCapture() {
  CriticalSectionScoped cs(&_apiCs);
  try {
    device_->StopCapture();
  } catch (Platform::Exception^ e) {
    int message_size = WideCharToMultiByte(
      CP_UTF8, 0, e->Message->Data(),
      static_cast<int>(wcslen(e->Message->Data())),
      NULL, 0, NULL, NULL);
    std::string message(message_size, 0);
    WideCharToMultiByte(
      CP_UTF8, 0, e->Message->Data(),
      static_cast<int>(wcslen(e->Message->Data())),
      &message[0], message_size, NULL, NULL);
    LOG(LS_ERROR) << "Failed to stop capture. " << message.c_str();
    return -1;
  }
  return 0;
}

bool VideoCaptureWinRT::CaptureStarted() {
  CriticalSectionScoped cs(&_apiCs);
  return device_->CaptureStarted();
}

int32_t VideoCaptureWinRT::CaptureSettings(VideoCaptureCapability& settings) {
  CriticalSectionScoped cs(&_apiCs);
  settings = device_->GetFrameInfo();
  return 0;
}

void VideoCaptureWinRT::OnIncomingFrame(
  uint8_t* video_frame,
  size_t video_frame_length,
  const VideoCaptureCapability& frame_info) {
  IncomingFrame(video_frame, video_frame_length, frame_info);
}

void VideoCaptureWinRT::OnCaptureDeviceFailed(HRESULT code,
                                              Platform::String^ message) {
  CriticalSectionScoped cs(&_apiCs);
  if (device_ != nullptr && device_->CaptureStarted())
    device_->StopCapture();
  int message_string_size = WideCharToMultiByte(
    CP_UTF8, 0, message->Data(),
    static_cast<int>(wcslen(message->Data())),
    NULL, 0, NULL, NULL);
  std::string message_string(message_string_size, 0);
  WideCharToMultiByte(
    CP_UTF8, 0, message->Data(),
    static_cast<int>(wcslen(message->Data())),
    &message_string[0], message_string_size, NULL, NULL);
  LOG(LS_ERROR) << "Capture device failed. HRESULT: " <<
    code << " Mesage: " << message_string.c_str();
}

}  // namespace videocapturemodule
}  // namespace webrtc
