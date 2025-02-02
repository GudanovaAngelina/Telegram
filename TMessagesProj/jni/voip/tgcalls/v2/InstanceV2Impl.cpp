#include "v2/InstanceV2Impl.h"

#include "LogSinkImpl.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"
#include "v2/NativeNetworkingImpl.h"
#include "v2/Signaling.h"
#include "v2/ContentNegotiation.h"

#include "CodecSelectHelper.h"
#include "platform/PlatformInterface.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_decoder_multi_channel_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/audio_codecs/L16/audio_decoder_L16.h"
#include "api/audio_codecs/L16/audio_encoder_L16.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "system_wrappers/include/field_trial.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "call/call.h"
#include "api/call/audio_sink.h"
#include "modules/audio_processing/audio_buffer.h"
#include "absl/strings/match.h"
#include "pc/channel_manager.h"
#include "audio/audio_state.h"
#include "modules/audio_coding/neteq/default_neteq_factory.h"
#include "modules/audio_coding/include/audio_coding_module.h"
#include "api/candidate.h"
#include "api/jsep_ice_candidate.h"
#include "pc/used_ids.h"
#include "media/base/sdp_video_format_utils.h"
#include "pc/media_session.h"
#include "rtc_base/rtc_certificate_generator.h"

#include "AudioFrame.h"
#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"
#include "CodecSelectHelper.h"
#include "AudioDeviceHelper.h"
#include "SignalingEncryption.h"
#ifdef WEBRTC_IOS
#include "platform/darwin/iOS/tgcalls_audio_device_module_ios.h"
#endif
#include <random>
#include <sstream>

#include "third-party/json11.hpp"

namespace tgcalls {
namespace {

static VideoCaptureInterfaceObject *GetVideoCaptureAssumingSameThread(VideoCaptureInterface *videoCapture) {
    return videoCapture
        ? static_cast<VideoCaptureInterfaceImpl*>(videoCapture)->object()->getSyncAssumingSameThread()
        : nullptr;
}

class OutgoingAudioChannel : public sigslot::has_slots<> {
public:
    OutgoingAudioChannel(
        webrtc::Call *call,
        cricket::ChannelManager *channelManager,
        rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator,
        webrtc::LocalAudioSinkAdapter *audioSource,
        webrtc::RtpTransport *rtpTransport,
        signaling::MediaContent const &mediaContent,
        std::shared_ptr<Threads> threads
    ) :
    _threads(threads),
    _ssrc(mediaContent.ssrc),
    _call(call),
    _channelManager(channelManager),
    _audioSource(audioSource) {
        cricket::AudioOptions audioOptions;
        bool _disableOutgoingAudioProcessing = false;

        if (_disableOutgoingAudioProcessing) {
            audioOptions.echo_cancellation = false;
            audioOptions.noise_suppression = false;
            audioOptions.auto_gain_control = false;
            audioOptions.highpass_filter = false;
            audioOptions.typing_detection = false;
            audioOptions.experimental_agc = false;
            audioOptions.experimental_ns = false;
            audioOptions.residual_echo_detector = false;
        } else {
            audioOptions.echo_cancellation = true;
            audioOptions.noise_suppression = true;
        }
        
        std::ostringstream contentId;
        contentId << _ssrc;

        std::vector<std::string> streamIds;
        streamIds.push_back(contentId.str());

        _outgoingAudioChannel = _channelManager->CreateVoiceChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), contentId.str(), false, NativeNetworkingImpl::getDefaulCryptoOptions(), uniqueRandomIdGenerator, audioOptions);

        std::vector<cricket::AudioCodec> codecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            if (payloadType.name == "opus") {
                cricket::AudioCodec codec(payloadType.id, payloadType.name, payloadType.clockrate, 0, payloadType.channels);

                codec.SetParam(cricket::kCodecParamUseInbandFec, 1);
                codec.SetParam(cricket::kCodecParamPTime, 60);
                
                for (const auto &feedbackType : payloadType.feedbackTypes) {
                    codec.AddFeedbackParam(cricket::FeedbackParam(feedbackType.type, feedbackType.subtype));
                }

                codecs.push_back(std::move(codec));
                
                break;
            }
        }

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingAudioDescription->set_codecs(codecs);
        outgoingAudioDescription->set_bandwidth(-1);
        outgoingAudioDescription->AddStream(cricket::StreamParams::CreateLegacy(_ssrc));

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingAudioDescription->set_rtcp_mux(true);
        incomingAudioDescription->set_rtcp_reduced_size(true);
        incomingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingAudioDescription->set_codecs(codecs);
        incomingAudioDescription->set_bandwidth(-1);

        _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _outgoingAudioChannel->SetPayloadTypeDemuxingEnabled(false);
            _outgoingAudioChannel->SetLocalContent(outgoingAudioDescription.get(), webrtc::SdpType::kOffer, nullptr);
            _outgoingAudioChannel->SetRemoteContent(incomingAudioDescription.get(), webrtc::SdpType::kAnswer, nullptr);
        });

        setIsMuted(false);
    }

    ~OutgoingAudioChannel() {
        _outgoingAudioChannel->Enable(false);
        _channelManager->DestroyVoiceChannel(_outgoingAudioChannel);
        _outgoingAudioChannel = nullptr;
    }

    void setIsMuted(bool isMuted) {
        if (_isMuted != isMuted) {
            _isMuted = isMuted;

            _outgoingAudioChannel->Enable(!_isMuted);
            _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
                _outgoingAudioChannel->media_channel()->SetAudioSend(_ssrc, !_isMuted, nullptr, _audioSource);
            });
        }
    }
    
    uint32_t ssrc() const {
        return _ssrc;
    }
    
    void setMaxBitrate(int bitrate) {
        _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            webrtc::RtpParameters initialParameters = _outgoingAudioChannel->media_channel()->GetRtpSendParameters(_ssrc);
            webrtc::RtpParameters updatedParameters = initialParameters;
            
            if (updatedParameters.encodings.empty()) {
                updatedParameters.encodings.push_back(webrtc::RtpEncodingParameters());
            }
            
            updatedParameters.encodings[0].max_bitrate_bps = bitrate;
            
            if (initialParameters != updatedParameters) {
                _outgoingAudioChannel->media_channel()->SetRtpSendParameters(_ssrc, updatedParameters);
            }
        });
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    std::shared_ptr<Threads> _threads;
    uint32_t _ssrc = 0;
    webrtc::Call *_call = nullptr;
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::LocalAudioSinkAdapter *_audioSource = nullptr;
    cricket::VoiceChannel *_outgoingAudioChannel = nullptr;

    bool _isMuted = true;
};

class IncomingV2AudioChannel : public sigslot::has_slots<> {
public:
    IncomingV2AudioChannel(
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        signaling::MediaContent const &mediaContent,
        std::shared_ptr<Threads> threads) :
    _ssrc(mediaContent.ssrc),
    _channelManager(channelManager),
    _call(call) {
        _creationTimestamp = rtc::TimeMillis();

        cricket::AudioOptions audioOptions;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;
        audioOptions.audio_jitter_buffer_min_delay_ms = 50;
        
        std::ostringstream contentId;
        contentId << _ssrc;

        std::string streamId = contentId.str();

        _audioChannel = _channelManager->CreateVoiceChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), contentId.str(), false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, audioOptions);

        std::vector<cricket::AudioCodec> codecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            cricket::AudioCodec codec(payloadType.id, payloadType.name, payloadType.clockrate, 0, payloadType.channels);
            for (const auto &parameter : payloadType.parameters) {
                codec.SetParam(parameter.first, parameter.second);
            }
            for (const auto &feedbackType : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(cricket::FeedbackParam(feedbackType.type, feedbackType.subtype));
            }
            codecs.push_back(std::move(codec));
        }

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingAudioDescription->set_codecs(codecs);
        outgoingAudioDescription->set_bandwidth(-1);

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingAudioDescription->set_rtcp_mux(true);
        incomingAudioDescription->set_rtcp_reduced_size(true);
        incomingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        incomingAudioDescription->set_codecs(codecs);
        incomingAudioDescription->set_bandwidth(-1);
        cricket::StreamParams streamParams = cricket::StreamParams::CreateLegacy(mediaContent.ssrc);
        streamParams.set_stream_ids({ streamId });
        incomingAudioDescription->AddStream(streamParams);

        threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _audioChannel->SetPayloadTypeDemuxingEnabled(false);
            _audioChannel->SetLocalContent(outgoingAudioDescription.get(), webrtc::SdpType::kOffer, nullptr);
            _audioChannel->SetRemoteContent(incomingAudioDescription.get(), webrtc::SdpType::kAnswer, nullptr);
        });

        outgoingAudioDescription.reset();
        incomingAudioDescription.reset();

        //std::unique_ptr<AudioSinkImpl> audioLevelSink(new AudioSinkImpl(onAudioLevelUpdated, _ssrc, std::move(onAudioFrame)));
        //_audioChannel->media_channel()->SetRawAudioSink(ssrc.networkSsrc, std::move(audioLevelSink));

        _audioChannel->Enable(true);
    }

    ~IncomingV2AudioChannel() {
        _audioChannel->Enable(false);
        _channelManager->DestroyVoiceChannel(_audioChannel);
        _audioChannel = nullptr;
    }

    void setVolume(double value) {
        _audioChannel->media_channel()->SetOutputVolume(_ssrc, value);
    }

    void updateActivity() {
        _activityTimestamp = rtc::TimeMillis();
    }

    int64_t getActivity() {
        return _activityTimestamp;
    }
    
    uint32_t ssrc() const {
        return _ssrc;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    uint32_t _ssrc = 0;
    // Memory is managed by _channelManager
    cricket::VoiceChannel *_audioChannel = nullptr;
    // Memory is managed externally
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
    int64_t _creationTimestamp = 0;
    int64_t _activityTimestamp = 0;
};

class OutgoingVideoChannel : public sigslot::has_slots<>, public std::enable_shared_from_this<OutgoingVideoChannel> {
public:
    OutgoingVideoChannel(
        std::shared_ptr<Threads> threads,
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        webrtc::VideoBitrateAllocatorFactory *videoBitrateAllocatorFactory,
        std::function<void()> rotationUpdated,
        signaling::MediaContent const &mediaContent,
        bool isScreencast
    ) :
    _threads(threads),
    _mainSsrc(mediaContent.ssrc),
    _call(call),
    _channelManager(channelManager),
    _rotationUpdated(rotationUpdated) {
        cricket::VideoOptions videoOptions;
        videoOptions.is_screencast = isScreencast;
        
        std::ostringstream contentId;
        contentId << mediaContent.ssrc;
        
        _outgoingVideoChannel = _channelManager->CreateVideoChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), contentId.str(), false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, videoOptions, videoBitrateAllocatorFactory);

        std::vector<cricket::VideoCodec> unsortedCodecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            cricket::VideoCodec codec(payloadType.id, payloadType.name);
            for (const auto &parameter : payloadType.parameters) {
                codec.SetParam(parameter.first, parameter.second);
            }
            for (const auto &feedbackType : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(cricket::FeedbackParam(feedbackType.type, feedbackType.subtype));
            }
            unsortedCodecs.push_back(std::move(codec));
        }
        
        std::vector<std::string> codecPreferences = {
#ifndef WEBRTC_DISABLE_H265
            cricket::kH265CodecName,
#endif
            cricket::kH264CodecName
        };
        
        std::vector<cricket::VideoCodec> codecs;
        for (const auto &name : codecPreferences) {
            for (const auto &codec : unsortedCodecs) {
                if (codec.name == name) {
                    codecs.push_back(codec);
                }
            }
        }
        for (const auto &codec : unsortedCodecs) {
            if (std::find(codecs.begin(), codecs.end(), codec) == codecs.end()) {
                codecs.push_back(codec);
            }
        }

        auto outgoingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingVideoDescription->AddRtpHeaderExtension(rtpExtension);
        }

        outgoingVideoDescription->set_rtcp_mux(true);
        outgoingVideoDescription->set_rtcp_reduced_size(true);
        outgoingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingVideoDescription->set_codecs(codecs);
        outgoingVideoDescription->set_bandwidth(-1);

        cricket::StreamParams videoSendStreamParams;

        for (const auto &ssrcGroup : mediaContent.ssrcGroups) {
            for (auto ssrc : ssrcGroup.ssrcs) {
                if (!videoSendStreamParams.has_ssrc(ssrc)) {
                    videoSendStreamParams.ssrcs.push_back(ssrc);
                }
            }

            cricket::SsrcGroup mappedGroup(ssrcGroup.semantics, ssrcGroup.ssrcs);
            videoSendStreamParams.ssrc_groups.push_back(std::move(mappedGroup));
        }

        videoSendStreamParams.cname = "cname";

        outgoingVideoDescription->AddStream(videoSendStreamParams);

        auto incomingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingVideoDescription->set_rtcp_mux(true);
        incomingVideoDescription->set_rtcp_reduced_size(true);
        incomingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingVideoDescription->set_codecs(codecs);
        incomingVideoDescription->set_bandwidth(-1);

        threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _outgoingVideoChannel->SetPayloadTypeDemuxingEnabled(false);
            _outgoingVideoChannel->SetLocalContent(outgoingVideoDescription.get(), webrtc::SdpType::kOffer, nullptr);
            _outgoingVideoChannel->SetRemoteContent(incomingVideoDescription.get(), webrtc::SdpType::kAnswer, nullptr);

            webrtc::RtpParameters rtpParameters = _outgoingVideoChannel->media_channel()->GetRtpSendParameters(mediaContent.ssrc);
            
            if (isScreencast) {
                rtpParameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
            }

            _outgoingVideoChannel->media_channel()->SetRtpSendParameters(mediaContent.ssrc, rtpParameters);
        });

        _outgoingVideoChannel->Enable(false);

        threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _outgoingVideoChannel->media_channel()->SetVideoSend(mediaContent.ssrc, NULL, nullptr);
        });
    }

    ~OutgoingVideoChannel() {
        _outgoingVideoChannel->Enable(false);
        _channelManager->DestroyVideoChannel(_outgoingVideoChannel);
        _outgoingVideoChannel = nullptr;
    }

    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
        _videoCapture = videoCapture;

        if (_videoCapture) {
            _outgoingVideoChannel->Enable(true);
            auto videoCaptureImpl = GetVideoCaptureAssumingSameThread(_videoCapture.get());

            _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
                _outgoingVideoChannel->media_channel()->SetVideoSend(_mainSsrc, NULL, videoCaptureImpl->source());
            });

            const auto weak = std::weak_ptr<OutgoingVideoChannel>(shared_from_this());
            videoCaptureImpl->setRotationUpdated([threads = _threads, weak](int angle) {
                threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                    const auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    signaling::MediaStateMessage::VideoRotation videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
                    switch (angle) {
                        case 0: {
                            videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
                            break;
                        }
                        case 90: {
                            videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation90;
                            break;
                        }
                        case 180: {
                            videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation180;
                            break;
                        }
                        case 270: {
                            videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation270;
                            break;
                        }
                        default: {
                            videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
                            break;
                        }
                    }
                    if (strong->_videoRotation != videoRotation) {
                        strong->_videoRotation = videoRotation;
                        strong->_rotationUpdated();
                    }
                });
            });

            switch (videoCaptureImpl->getRotation()) {
                case 0: {
                    _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
                    break;
                }
                case 90: {
                    _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation90;
                    break;
                }
                case 180: {
                    _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation180;
                    break;
                }
                case 270: {
                    _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation270;
                    break;
                }
                default: {
                    _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
                    break;
                }
            }
        } else {
            _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
            _outgoingVideoChannel->Enable(false);

            _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
                _outgoingVideoChannel->media_channel()->SetVideoSend(_mainSsrc, NULL, nullptr);
            });
        }
    }
    
    uint32_t ssrc() const {
        return _mainSsrc;
    }
    
    void setMaxBitrate(int bitrate) {
        _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            webrtc::RtpParameters initialParameters = _outgoingVideoChannel->media_channel()->GetRtpSendParameters(_mainSsrc);
            webrtc::RtpParameters updatedParameters = initialParameters;
            
            if (updatedParameters.encodings.empty()) {
                updatedParameters.encodings.push_back(webrtc::RtpEncodingParameters());
            }
            
            updatedParameters.encodings[0].max_bitrate_bps = bitrate;
            
            if (initialParameters != updatedParameters) {
                _outgoingVideoChannel->media_channel()->SetRtpSendParameters(_mainSsrc, updatedParameters);
            }
        });
    }

public:
    std::shared_ptr<VideoCaptureInterface> videoCapture() {
        return _videoCapture;
    }

    signaling::MediaStateMessage::VideoRotation getRotation() {
        return _videoRotation;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    std::shared_ptr<Threads> _threads;

    uint32_t _mainSsrc = 0;
    webrtc::Call *_call = nullptr;
    cricket::ChannelManager *_channelManager = nullptr;
    cricket::VideoChannel *_outgoingVideoChannel = nullptr;

    std::function<void()> _rotationUpdated;

    std::shared_ptr<VideoCaptureInterface> _videoCapture;
    signaling::MediaStateMessage::VideoRotation _videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
};

class VideoSinkImpl : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    VideoSinkImpl() {
    }

    virtual ~VideoSinkImpl() {
    }

    virtual void OnFrame(const webrtc::VideoFrame& frame) override {
        //_lastFrame = frame;
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnFrame(frame);
            }
        }
    }

    virtual void OnDiscardedFrame() override {
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnDiscardedFrame();
            }
        }
    }

    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _sinks.push_back(impl);
        if (_lastFrame) {
            auto strong = impl.lock();
            if (strong) {
                strong->OnFrame(_lastFrame.value());
            }
        }
    }

private:
    std::vector<std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>> _sinks;
    absl::optional<webrtc::VideoFrame> _lastFrame;
};

class IncomingV2VideoChannel : public sigslot::has_slots<> {
public:
    IncomingV2VideoChannel(
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        signaling::MediaContent const &mediaContent,
        std::shared_ptr<Threads> threads) :
    _channelManager(channelManager),
    _call(call) {
        _videoSink.reset(new VideoSinkImpl());

        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
        
        std::ostringstream contentId;
        contentId << mediaContent.ssrc;

        _videoChannel = _channelManager->CreateVideoChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), contentId.str(), false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, cricket::VideoOptions(), _videoBitrateAllocatorFactory.get());

        std::vector<cricket::VideoCodec> codecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            cricket::VideoCodec codec(payloadType.id, payloadType.name);
            for (const auto &parameter : payloadType.parameters) {
                codec.SetParam(parameter.first, parameter.second);
            }
            for (const auto &feedbackType : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(cricket::FeedbackParam(feedbackType.type, feedbackType.subtype));
            }
            codecs.push_back(std::move(codec));
        }

        auto outgoingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingVideoDescription->set_rtcp_mux(true);
        outgoingVideoDescription->set_rtcp_reduced_size(true);
        outgoingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingVideoDescription->set_codecs(codecs);
        outgoingVideoDescription->set_bandwidth(-1);

        cricket::StreamParams videoRecvStreamParams;

        _mainVideoSsrc = mediaContent.ssrc;

        std::vector<uint32_t> allSsrcs;
        for (const auto &group : mediaContent.ssrcGroups) {
            for (auto ssrc : group.ssrcs) {
                if (std::find(allSsrcs.begin(), allSsrcs.end(), ssrc) == allSsrcs.end()) {
                    allSsrcs.push_back(ssrc);
                }
            }

            cricket::SsrcGroup parsedGroup(group.semantics, group.ssrcs);
            videoRecvStreamParams.ssrc_groups.push_back(parsedGroup);
        }
        videoRecvStreamParams.ssrcs = allSsrcs;

        videoRecvStreamParams.cname = "cname";
        videoRecvStreamParams.set_stream_ids({ contentId.str() });

        auto incomingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingVideoDescription->set_rtcp_mux(true);
        incomingVideoDescription->set_rtcp_reduced_size(true);
        incomingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        incomingVideoDescription->set_codecs(codecs);
        incomingVideoDescription->set_bandwidth(-1);

        incomingVideoDescription->AddStream(videoRecvStreamParams);

        threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _videoChannel->SetPayloadTypeDemuxingEnabled(false);
            _videoChannel->SetLocalContent(outgoingVideoDescription.get(), webrtc::SdpType::kOffer, nullptr);
            _videoChannel->SetRemoteContent(incomingVideoDescription.get(), webrtc::SdpType::kAnswer, nullptr);

            _videoChannel->media_channel()->SetSink(_mainVideoSsrc, _videoSink.get());
        });

        _videoChannel->Enable(true);
    }

    ~IncomingV2VideoChannel() {
        _videoChannel->Enable(false);
        _channelManager->DestroyVideoChannel(_videoChannel);
        _videoChannel = nullptr;
    }

    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _videoSink->addSink(impl);
    }
    
    uint32_t ssrc() const {
        return _mainVideoSsrc;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    uint32_t _mainVideoSsrc = 0;
    std::unique_ptr<VideoSinkImpl> _videoSink;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;
    // Memory is managed by _channelManager
    cricket::VideoChannel *_videoChannel;
    // Memory is managed externally
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
};

template<typename T>
struct StateLogRecord {
    int64_t timestamp = 0;
    T record;
    
    explicit StateLogRecord(int32_t timestamp_, T &&record_) :
    timestamp(timestamp_),
    record(std::move(record_)) {
    }
};

struct NetworkStateLogRecord {
    bool isConnected = false;
    bool isFailed = false;
    absl::optional<NativeNetworkingImpl::RouteDescription> route;
    absl::optional<NativeNetworkingImpl::ConnectionDescription> connection;
    
    bool operator==(NetworkStateLogRecord const &rhs) const {
        if (isConnected != rhs.isConnected) {
            return false;
        }
        if (isFailed != rhs.isFailed) {
            return false;
        }
        if (route != rhs.route) {
            return false;
        }
        if (connection != rhs.connection) {
            return false;
        }
        
        return true;
    }
};

struct NetworkBitrateLogRecord {
    int32_t bitrate = 0;
};

} // namespace

class InstanceV2ImplInternal : public std::enable_shared_from_this<InstanceV2ImplInternal> {
public:
    InstanceV2ImplInternal(Descriptor &&descriptor, std::shared_ptr<Threads> threads) :
    _threads(threads),
    _rtcServers(descriptor.rtcServers),
    _proxy(std::move(descriptor.proxy)),
    _enableP2P(descriptor.config.enableP2P),
    _encryptionKey(std::move(descriptor.encryptionKey)),
    _stateUpdated(descriptor.stateUpdated),
    _signalBarsUpdated(descriptor.signalBarsUpdated),
    _audioLevelUpdated(descriptor.audioLevelUpdated),
    _remoteBatteryLevelIsLowUpdated(descriptor.remoteBatteryLevelIsLowUpdated),
    _remoteMediaStateUpdated(descriptor.remoteMediaStateUpdated),
    _remotePrefferedAspectRatioUpdated(descriptor.remotePrefferedAspectRatioUpdated),
    _signalingDataEmitted(descriptor.signalingDataEmitted),
    _createAudioDeviceModule(descriptor.createAudioDeviceModule),
    _statsLogPath(descriptor.config.statsLogPath),
    _eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
    _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()),
    _videoCapture(descriptor.videoCapture),
    _platformContext(descriptor.platformContext) {
    }

    ~InstanceV2ImplInternal() {
        _networking->perform(RTC_FROM_HERE, [](NativeNetworkingImpl *networking) {
            networking->stop();
        });

        _incomingAudioChannel.reset();
        _incomingVideoChannel.reset();
        _incomingScreencastChannel.reset();
        _outgoingAudioChannel.reset();
        _outgoingVideoChannel.reset();
        _outgoingScreencastChannel.reset();
        _currentSink.reset();

        _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            _channelManager.reset();
            _call.reset();
            _audioDeviceModule = nullptr;
        });
        
        _contentNegotiationContext.reset();
        
        _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, []() {
        });
    }

    void start() {
        _startTimestamp = rtc::TimeMillis();
        
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());
        
        absl::optional<Proxy> proxy;
        if (_proxy) {
            proxy = *(_proxy.get());
        }

        _networking.reset(new ThreadLocalObject<NativeNetworkingImpl>(_threads->getNetworkThread(), [weak, threads = _threads, isOutgoing = _encryptionKey.isOutgoing, rtcServers = _rtcServers, proxy, enableP2P = _enableP2P]() {
            return new NativeNetworkingImpl(NativeNetworkingImpl::Configuration{
                .isOutgoing = isOutgoing,
                .enableStunMarking = false,
                .enableTCP = false,
                .enableP2P = enableP2P,
                .rtcServers = rtcServers,
                .proxy = proxy,
                .stateUpdated = [threads, weak](const NativeNetworkingImpl::State &state) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onNetworkStateUpdated(state);
                    });
                },
                .candidateGathered = [threads, weak](const cricket::Candidate &candidate) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }

                        strong->sendCandidate(candidate);
                    });
                },
                .transportMessageReceived = [threads, weak](rtc::CopyOnWriteBuffer const &packet, bool isMissing) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                    });
                },
                .rtcpPacketReceived = [threads, weak](rtc::CopyOnWriteBuffer const &packet, int64_t timestamp) {
                    const auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->_call->Receiver()->DeliverPacket(webrtc::MediaType::ANY, packet, timestamp);
                },
                .dataChannelStateUpdated = [threads, weak](bool isDataChannelOpen) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onDataChannelStateUpdated(isDataChannelOpen);
                    });
                },
                .dataChannelMessageReceived = [threads, weak](std::string const &message) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onDataChannelMessage(message);
                    });
                },
                .threads = threads
            });
        }));

        PlatformInterface::SharedInstance()->configurePlatformAudio();

        //setAudioInputDevice(_initialInputDeviceId);
        //setAudioOutputDevice(_initialOutputDeviceId);

        _threads->getWorkerThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
            cricket::MediaEngineDependencies mediaDeps;
            mediaDeps.task_queue_factory = _taskQueueFactory.get();
            mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus, webrtc::AudioEncoderL16>();
            mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus, webrtc::AudioDecoderL16>();

            mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory(_platformContext, true);
            mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory(_platformContext);

            _audioDeviceModule = createAudioDeviceModule();
            /*if (!_audioDeviceModule) {
                return;
            }*/
            mediaDeps.adm = _audioDeviceModule;

            _availableVideoFormats = mediaDeps.video_encoder_factory->GetSupportedFormats();

            std::unique_ptr<cricket::MediaEngineInterface> mediaEngine = cricket::CreateMediaEngine(std::move(mediaDeps));

            _channelManager = cricket::ChannelManager::Create(
                std::move(mediaEngine),
                true,
                _threads->getWorkerThread(),
                _threads->getNetworkThread()
            );

            webrtc::Call::Config callConfig(_eventLog.get());
            callConfig.task_queue_factory = _taskQueueFactory.get();
            callConfig.trials = &_fieldTrials;
            callConfig.audio_state = _channelManager->media_engine()->voice().GetAudioState();

            _call.reset(webrtc::Call::Create(callConfig, webrtc::Clock::GetRealTimeClock(), _threads->getSharedModuleThread(), webrtc::ProcessThread::Create("PacerThread")));
        });

        _uniqueRandomIdGenerator.reset(new rtc::UniqueRandomIdGenerator());
        
        _contentNegotiationContext = std::make_unique<ContentNegotiationContext>(_encryptionKey.isOutgoing, _uniqueRandomIdGenerator.get());
        _contentNegotiationContext->copyCodecsFromChannelManager(_channelManager.get(), false);
        
        _outgoingAudioChannelId = _contentNegotiationContext->addOutgoingChannel(signaling::MediaContent::Type::Audio);
        //_contentNegotiationContext->addOutgoingChannel(signaling::MediaContent::Type::Video);

        _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [this]() {
            _rtpTransport = _networking->getSyncAssumingSameThread()->getRtpTransport();
        });

        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();

        _networking->perform(RTC_FROM_HERE, [](NativeNetworkingImpl *networking) {
            networking->start();
        });

        if (_videoCapture) {
            setVideoCapture(_videoCapture);
        }

        beginSignaling();

        adjustBitratePreferences(true);
        
        beginQualityTimer(0);
        beginLogTimer(0);
    }
    
    void beginQualityTimer(int delayMs) {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());
        _threads->getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }

            
            
            strong->beginQualityTimer(500);
        }, delayMs);
    }
    
    void beginLogTimer(int delayMs) {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());
        _threads->getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }

            strong->writeStateLogRecords();

            strong->beginLogTimer(1000);
        }, delayMs);
    }
    
    void writeStateLogRecords() {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());
        _threads->getWorkerThread()->PostTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }
            
            auto stats = strong->_call->GetStats();
            float sendBitrateKbps = ((float)stats.send_bandwidth_bps / 1000.0f);
            
            strong->_threads->getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sendBitrateKbps]() {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                float bitrateNorm = 16.0f;
                if (strong->_outgoingVideoChannel) {
                    bitrateNorm = 600.0f;
                }
                
                float signalBarsNorm = 4.0f;
                float adjustedQuality = sendBitrateKbps / bitrateNorm;
                adjustedQuality = fmaxf(0.0f, adjustedQuality);
                adjustedQuality = fminf(1.0f, adjustedQuality);
                if (strong->_signalBarsUpdated) {
                    strong->_signalBarsUpdated((int)(adjustedQuality * signalBarsNorm));
                }
                
                NetworkBitrateLogRecord networkBitrateLogRecord;
                networkBitrateLogRecord.bitrate = (int32_t)sendBitrateKbps;
                
                strong->_networkBitrateLogRecords.emplace_back(rtc::TimeMillis(), std::move(networkBitrateLogRecord));
            });
        });
    }

    void sendSignalingMessage(signaling::Message const &message) {
        auto data = message.serialize();

        RTC_LOG(LS_INFO) << "sendSignalingMessage: " << std::string(data.begin(), data.end());

        if (_signalingEncryption) {
            if (const auto encryptedData = _signalingEncryption->encryptOutgoing(data)) {
                _signalingDataEmitted(std::vector<uint8_t>(encryptedData->data(), encryptedData->data() + encryptedData->size()));
            } else {
                RTC_LOG(LS_ERROR) << "sendSignalingMessage: failed to encrypt payload";
            }
        } else {
            _signalingDataEmitted(data);
        }
    }

    void beginSignaling() {
        _signalingEncryption.reset(new SignalingEncryption(_encryptionKey));

        if (_encryptionKey.isOutgoing) {
            sendInitialSetup();
        }
    }

    void createNegotiatedChannels() {
        const auto coordinatedState = _contentNegotiationContext->coordinatedState();
        if (!coordinatedState) {
            return;
        }
        
        if (_outgoingAudioChannelId) {
            const auto audioSsrc = _contentNegotiationContext->outgoingChannelSsrc(_outgoingAudioChannelId.value());
            if (audioSsrc) {
                if (_outgoingAudioChannel && _outgoingAudioChannel->ssrc() != audioSsrc.value()) {
                    _outgoingAudioChannel.reset();
                }
                
                absl::optional<signaling::MediaContent> outgoingAudioContent;
                for (const auto &content : coordinatedState->outgoingContents) {
                    if (content.type == signaling::MediaContent::Type::Audio && content.ssrc == audioSsrc.value()) {
                        outgoingAudioContent = content;
                        break;
                    }
                }
                
                if (outgoingAudioContent) {
                    if (!_outgoingAudioChannel) {
                        _outgoingAudioChannel.reset(new OutgoingAudioChannel(
                            _call.get(),
                            _channelManager.get(),
                            _uniqueRandomIdGenerator.get(),
                            &_audioSource,
                            _rtpTransport,
                            outgoingAudioContent.value(),
                            _threads
                        ));
                    }
                }
            }
        }
        
        if (_outgoingVideoChannelId) {
            const auto videoSsrc = _contentNegotiationContext->outgoingChannelSsrc(_outgoingVideoChannelId.value());
            if (videoSsrc) {
                if (_outgoingVideoChannel && _outgoingVideoChannel->ssrc() != videoSsrc.value()) {
                    _outgoingVideoChannel.reset();
                }
                
                absl::optional<signaling::MediaContent> outgoingVideoContent;
                for (const auto &content : coordinatedState->outgoingContents) {
                    if (content.type == signaling::MediaContent::Type::Video && content.ssrc == videoSsrc.value()) {
                        outgoingVideoContent = content;
                        break;
                    }
                }
                
                if (outgoingVideoContent) {
                    if (!_outgoingVideoChannel) {
                        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

                        _outgoingVideoChannel.reset(new OutgoingVideoChannel(
                            _threads,
                            _channelManager.get(),
                            _call.get(),
                            _rtpTransport,
                            _uniqueRandomIdGenerator.get(),
                            _videoBitrateAllocatorFactory.get(),
                            [threads = _threads, weak]() {
                                threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                                    const auto strong = weak.lock();
                                    if (!strong) {
                                        return;
                                    }
                                    strong->sendMediaState();
                                });
                            },
                            outgoingVideoContent.value(),
                            false
                        ));

                        if (_videoCapture) {
                            _outgoingVideoChannel->setVideoCapture(_videoCapture);
                        }
                    }
                }
            }
        }
        
        if (_outgoingScreencastChannelId) {
            const auto screencastSsrc = _contentNegotiationContext->outgoingChannelSsrc(_outgoingScreencastChannelId.value());
            if (screencastSsrc) {
                if (_outgoingScreencastChannel && _outgoingScreencastChannel->ssrc() != screencastSsrc.value()) {
                    _outgoingScreencastChannel.reset();
                }
                
                absl::optional<signaling::MediaContent> outgoingScreencastContent;
                for (const auto &content : coordinatedState->outgoingContents) {
                    if (content.type == signaling::MediaContent::Type::Video && content.ssrc == screencastSsrc.value()) {
                        outgoingScreencastContent = content;
                        break;
                    }
                }
                
                if (outgoingScreencastContent) {
                    if (!_outgoingScreencastChannel) {
                        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

                        _outgoingScreencastChannel.reset(new OutgoingVideoChannel(
                            _threads,
                            _channelManager.get(),
                            _call.get(),
                            _rtpTransport,
                            _uniqueRandomIdGenerator.get(),
                            _videoBitrateAllocatorFactory.get(),
                            [threads = _threads, weak]() {
                                threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                                    const auto strong = weak.lock();
                                    if (!strong) {
                                        return;
                                    }
                                    strong->sendMediaState();
                                });
                            },
                            outgoingScreencastContent.value(),
                            true
                        ));

                        if (_screencastCapture) {
                            _outgoingScreencastChannel->setVideoCapture(_screencastCapture);
                        }
                    }
                }
            }
        }
        
        for (const auto &content : coordinatedState->incomingContents) {
            switch (content.type) {
                case signaling::MediaContent::Type::Audio: {
                    if (_incomingAudioChannel && _incomingAudioChannel->ssrc() != content.ssrc) {
                        _incomingAudioChannel.reset();
                    }
                    
                    if (!_incomingAudioChannel) {
                        _incomingAudioChannel.reset(new IncomingV2AudioChannel(
                            _channelManager.get(),
                            _call.get(),
                            _rtpTransport,
                            _uniqueRandomIdGenerator.get(),
                            content,
                            _threads
                        ));
                    }
                    
                    break;
                }
                case signaling::MediaContent::Type::Video: {
                    if (_incomingVideoChannel && _incomingVideoChannel->ssrc() != content.ssrc) {
                        _incomingVideoChannel.reset();
                    }
                    
                    if (!_incomingVideoChannel) {
                        _incomingVideoChannel.reset(new IncomingV2VideoChannel(
                            _channelManager.get(),
                            _call.get(),
                            _rtpTransport,
                            _uniqueRandomIdGenerator.get(),
                            content,
                            _threads
                        ));
                        _incomingVideoChannel->addSink(_currentSink);
                    }
                    
                    break;
                }
                default: {
                    RTC_FATAL() << "Unknown media type";
                    break;
                }
            }
        }
        
        /*

        if (_negotiatedOutgoingScreencastContent) {
            const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

            _outgoingScreencastChannel.reset(new OutgoingVideoChannel(
                _threads,
                _channelManager.get(),
                _call.get(),
                _rtpTransport,
                _uniqueRandomIdGenerator.get(),
                _videoBitrateAllocatorFactory.get(),
                [threads = _threads, weak]() {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->sendMediaState();
                    });
                },
                _negotiatedOutgoingScreencastContent.value(),
                true
            ));

            if (_screencastCapture) {
                _outgoingScreencastChannel->setVideoCapture(_screencastCapture);
            }
        }

        if (_negotiatedOutgoingAudioContent) {
            _outgoingAudioChannel.reset(new OutgoingAudioChannel(
                _call.get(),
                _channelManager.get(),
                _uniqueRandomIdGenerator.get(),
                &_audioSource,
                _rtpTransport,
                _negotiatedOutgoingAudioContent.value(),
                _threads
            ));
        }*/

        adjustBitratePreferences(true);
        sendMediaState();
    }

    void sendInitialSetup() {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

        _networking->perform(RTC_FROM_HERE, [weak, threads = _threads, isOutgoing = _encryptionKey.isOutgoing](NativeNetworkingImpl *networking) {
            auto localFingerprint = networking->getLocalFingerprint();
            std::string hash = localFingerprint->algorithm;
            std::string fingerprint = localFingerprint->GetRfc4572Fingerprint();
            std::string setup;
            if (isOutgoing) {
                setup = "actpass";
            } else {
                setup = "passive";
            }

            auto localIceParams = networking->getLocalIceParameters();
            std::string ufrag = localIceParams.ufrag;
            std::string pwd = localIceParams.pwd;

            threads->getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ufrag, pwd, hash, fingerprint, setup, localIceParams]() {
                const auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                signaling::InitialSetupMessage data;

                data.ufrag = ufrag;
                data.pwd = pwd;

                signaling::DtlsFingerprint dtlsFingerprint;
                dtlsFingerprint.hash = hash;
                dtlsFingerprint.fingerprint = fingerprint;
                dtlsFingerprint.setup = setup;
                data.fingerprints.push_back(std::move(dtlsFingerprint));

                signaling::Message message;
                message.data = std::move(data);
                strong->sendSignalingMessage(message);
            });
        });
    }
    
    void sendOfferIfNeeded() {
        if (const auto offer = _contentNegotiationContext->getPendingOffer()) {
            signaling::NegotiateChannelsMessage data;

            data.exchangeId = offer->exchangeId;
            
            data.contents = offer->contents;

            signaling::Message message;
            message.data = std::move(data);
            sendSignalingMessage(message);
        }
    }

    void receiveSignalingData(const std::vector<uint8_t> &data) {
        std::vector<uint8_t> decryptedData;

        if (_signalingEncryption) {
            const auto rawDecryptedData = _signalingEncryption->decryptIncoming(data);
            if (!rawDecryptedData) {
                RTC_LOG(LS_ERROR) << "receiveSignalingData: could not decrypt payload";

                return;
            }

            decryptedData = std::vector<uint8_t>(rawDecryptedData->data(), rawDecryptedData->data() + rawDecryptedData->size());
        } else {
            decryptedData = data;
        }

        processSignalingData(decryptedData);
    }

    void processSignalingData(const std::vector<uint8_t> &data) {
        RTC_LOG(LS_INFO) << "processSignalingData: " << std::string(data.begin(), data.end());

        const auto message = signaling::Message::parse(data);
        if (!message) {
            return;
        }
        const auto messageData = &message->data;
        if (const auto initialSetup = absl::get_if<signaling::InitialSetupMessage>(messageData)) {
            PeerIceParameters remoteIceParameters;
            remoteIceParameters.ufrag = initialSetup->ufrag;
            remoteIceParameters.pwd = initialSetup->pwd;

            std::unique_ptr<rtc::SSLFingerprint> fingerprint;
            std::string sslSetup;
            if (initialSetup->fingerprints.size() != 0) {
                fingerprint = rtc::SSLFingerprint::CreateUniqueFromRfc4572(initialSetup->fingerprints[0].hash, initialSetup->fingerprints[0].fingerprint);
                sslSetup = initialSetup->fingerprints[0].setup;
            }

            _networking->perform(RTC_FROM_HERE, [threads = _threads, remoteIceParameters = std::move(remoteIceParameters), fingerprint = std::move(fingerprint), sslSetup = std::move(sslSetup)](NativeNetworkingImpl *networking) {
                networking->setRemoteParams(remoteIceParameters, fingerprint.get(), sslSetup);
            });
            
            _handshakeCompleted = true;

            if (_encryptionKey.isOutgoing) {
                sendOfferIfNeeded();
            } else {
                sendInitialSetup();
            }

            commitPendingIceCandidates();
        } else if (const auto offerAnwer = absl::get_if<signaling::NegotiateChannelsMessage>(messageData)) {
            auto negotiationContents = std::make_unique<ContentNegotiationContext::NegotiationContents>();
            negotiationContents->exchangeId = offerAnwer->exchangeId;
            negotiationContents->contents = offerAnwer->contents;
            
            if (const auto response = _contentNegotiationContext->setRemoteNegotiationContent(std::move(negotiationContents))) {
                signaling::NegotiateChannelsMessage data;

                data.exchangeId = response->exchangeId;
                data.contents = response->contents;

                signaling::Message message;
                message.data = std::move(data);
                sendSignalingMessage(message);
            }
            
            sendOfferIfNeeded();
            
            createNegotiatedChannels();
        } else if (const auto candidatesList = absl::get_if<signaling::CandidatesMessage>(messageData)) {
            for (const auto &candidate : candidatesList->iceCandidates) {
                webrtc::JsepIceCandidate parseCandidate{ std::string(), 0 };
                if (!parseCandidate.Initialize(candidate.sdpString, nullptr)) {
                    RTC_LOG(LS_ERROR) << "Could not parse candidate: " << candidate.sdpString;
                    continue;
                }
                _pendingIceCandidates.push_back(parseCandidate.candidate());
            }

            if (_handshakeCompleted) {
                commitPendingIceCandidates();
            }
        } else if (const auto mediaState = absl::get_if<signaling::MediaStateMessage>(messageData)) {
            AudioState mappedAudioState;
            if (mediaState->isMuted) {
                mappedAudioState = AudioState::Muted;
            } else {
                mappedAudioState = AudioState::Active;
            }

            VideoState mappedVideoState;
            switch (mediaState->videoState) {
                case signaling::MediaStateMessage::VideoState::Inactive: {
                    mappedVideoState = VideoState::Inactive;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Suspended: {
                    mappedVideoState = VideoState::Paused;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Active: {
                    mappedVideoState = VideoState::Active;
                    break;
                }
                default: {
                    RTC_FATAL() << "Unknown videoState";
                    break;
                }
            }

            VideoState mappedScreencastState;
            switch (mediaState->screencastState) {
                case signaling::MediaStateMessage::VideoState::Inactive: {
                    mappedScreencastState = VideoState::Inactive;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Suspended: {
                    mappedScreencastState = VideoState::Paused;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Active: {
                    mappedScreencastState = VideoState::Active;
                    break;
                }
                default: {
                    RTC_FATAL() << "Unknown videoState";
                    break;
                }
            }

            VideoState effectiveVideoState = mappedVideoState;
            if (mappedScreencastState == VideoState::Active || mappedScreencastState == VideoState::Paused) {
                effectiveVideoState = mappedScreencastState;
            }

            if (_remoteMediaStateUpdated) {
                _remoteMediaStateUpdated(mappedAudioState, effectiveVideoState);
            }

            if (_remoteBatteryLevelIsLowUpdated) {
                _remoteBatteryLevelIsLowUpdated(mediaState->isBatteryLow);
            }
        }
    }

    void commitPendingIceCandidates() {
        if (_pendingIceCandidates.size() == 0) {
            return;
        }
        _networking->perform(RTC_FROM_HERE, [threads = _threads, parsedCandidates = _pendingIceCandidates](NativeNetworkingImpl *networking) {
            networking->addCandidates(parsedCandidates);
        });
        _pendingIceCandidates.clear();
    }

    void onNetworkStateUpdated(NativeNetworkingImpl::State const &state) {
        State mappedState;
        if (state.isFailed) {
            mappedState = State::Failed;
        } else if (state.isReadyToSendData) {
            mappedState = State::Established;
        } else {
            mappedState = State::Reconnecting;
        }
        
        NetworkStateLogRecord record;
        record.isConnected = state.isReadyToSendData;
        record.route = state.route;
        record.connection = state.connection;
        record.isFailed = state.isFailed;
        
        if (!_currentNetworkStateLogRecord || !(_currentNetworkStateLogRecord.value() == record)) {
            _currentNetworkStateLogRecord = record;
            _networkStateLogRecords.emplace_back(rtc::TimeMillis(), std::move(record));
        }
        
        _networkState = state;
        _stateUpdated(mappedState);
    }

    void onDataChannelStateUpdated(bool isDataChannelOpen) {
        if (_isDataChannelOpen != isDataChannelOpen) {
            _isDataChannelOpen = isDataChannelOpen;

            if (_isDataChannelOpen) {
                sendMediaState();
            }
        }
    }

    void sendDataChannelMessage(signaling::Message const &message) {
        if (!_isDataChannelOpen) {
            RTC_LOG(LS_ERROR) << "sendDataChannelMessage called, but data channel is not open";
            return;
        }
        auto data = message.serialize();
        std::string stringData(data.begin(), data.end());
        RTC_LOG(LS_INFO) << "sendDataChannelMessage: " << stringData;
        _networking->perform(RTC_FROM_HERE, [stringData = std::move(stringData)](NativeNetworkingImpl *networking) {
            networking->sendDataChannelMessage(stringData);
        });
    }

    void onDataChannelMessage(std::string const &message) {
        RTC_LOG(LS_INFO) << "dataChannelMessage received: " << message;
        std::vector<uint8_t> data(message.begin(), message.end());
        processSignalingData(data);
    }

    void sendMediaState() {
        if (!_isDataChannelOpen) {
            return;
        }
        signaling::Message message;
        signaling::MediaStateMessage data;
        data.isMuted = _isMicrophoneMuted;
        data.isBatteryLow = _isBatteryLow;
        if (_outgoingVideoChannel) {
            if (_outgoingVideoChannel->videoCapture()) {
                data.videoState = signaling::MediaStateMessage::VideoState::Active;
            } else{
                data.videoState = signaling::MediaStateMessage::VideoState::Inactive;
            }
            data.videoRotation = _outgoingVideoChannel->getRotation();
        } else {
            data.videoState = signaling::MediaStateMessage::VideoState::Inactive;
            data.videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
        }
        if (_outgoingScreencastChannel) {
            if (_outgoingScreencastChannel->videoCapture()) {
                data.screencastState = signaling::MediaStateMessage::VideoState::Active;
            } else{
                data.screencastState = signaling::MediaStateMessage::VideoState::Inactive;
            }
        } else {
            data.screencastState = signaling::MediaStateMessage::VideoState::Inactive;
        }
        message.data = std::move(data);
        sendDataChannelMessage(message);
    }

    void sendCandidate(const cricket::Candidate &candidate) {
        cricket::Candidate patchedCandidate = candidate;
        patchedCandidate.set_component(1);

        signaling::CandidatesMessage data;

        signaling::IceCandidate serializedCandidate;

        webrtc::JsepIceCandidate iceCandidate{ std::string(), 0 };
        iceCandidate.SetCandidate(patchedCandidate);
        std::string serialized;
        const auto success = iceCandidate.ToString(&serialized);
        assert(success);
        (void)success;

        serializedCandidate.sdpString = serialized;

        data.iceCandidates.push_back(std::move(serializedCandidate));

        signaling::Message message;
        message.data = std::move(data);
        sendSignalingMessage(message);
    }

    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
        auto videoCaptureImpl = GetVideoCaptureAssumingSameThread(videoCapture.get());
        if (videoCaptureImpl) {
            if (videoCaptureImpl->isScreenCapture()) {
                _videoCapture = nullptr;
                _screencastCapture = videoCapture;

                if (_outgoingVideoChannel) {
                    _outgoingVideoChannel->setVideoCapture(nullptr);
                }
                if (_outgoingVideoChannelId) {
                    _contentNegotiationContext->removeOutgoingChannel(_outgoingVideoChannelId.value());
                    _outgoingVideoChannelId.reset();
                }

                if (_outgoingScreencastChannel) {
                    _outgoingScreencastChannel->setVideoCapture(videoCapture);
                }
                if (!_outgoingScreencastChannelId) {
                    _outgoingScreencastChannelId = _contentNegotiationContext->addOutgoingChannel(signaling::MediaContent::Type::Video);
                }
            } else {
                _videoCapture = videoCapture;
                _screencastCapture = nullptr;

                if (_outgoingVideoChannel) {
                    _outgoingVideoChannel->setVideoCapture(videoCapture);
                }
                if (!_outgoingVideoChannelId) {
                    _outgoingVideoChannelId = _contentNegotiationContext->addOutgoingChannel(signaling::MediaContent::Type::Video);
                }

                if (_outgoingScreencastChannel) {
                    _outgoingScreencastChannel->setVideoCapture(nullptr);
                }
                if (_outgoingScreencastChannelId) {
                    _contentNegotiationContext->removeOutgoingChannel(_outgoingScreencastChannelId.value());
                    _outgoingScreencastChannelId.reset();
                }
            }
        } else {
            _videoCapture = nullptr;
            _screencastCapture = nullptr;

            if (_outgoingVideoChannel) {
                _outgoingVideoChannel->setVideoCapture(nullptr);
            }

            if (_outgoingScreencastChannel) {
                _outgoingScreencastChannel->setVideoCapture(nullptr);
            }
            
            if (_outgoingVideoChannelId) {
                _contentNegotiationContext->removeOutgoingChannel(_outgoingVideoChannelId.value());
                _outgoingVideoChannelId.reset();
            }
            
            if (_outgoingScreencastChannelId) {
                _contentNegotiationContext->removeOutgoingChannel(_outgoingScreencastChannelId.value());
                _outgoingScreencastChannelId.reset();
            }
        }

        if (_handshakeCompleted) {
            sendOfferIfNeeded();
            sendMediaState();
            adjustBitratePreferences(true);
            createNegotiatedChannels();
        }
    }

    void setRequestedVideoAspect(float aspect) {
    }

    void setNetworkType(NetworkType networkType) {

    }

    void setMuteMicrophone(bool muteMicrophone) {
        if (_isMicrophoneMuted != muteMicrophone) {
            _isMicrophoneMuted = muteMicrophone;

            if (_outgoingAudioChannel) {
                _outgoingAudioChannel->setIsMuted(muteMicrophone);
            }

            sendMediaState();
        }
    }

    void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
        _currentSink = sink;
        if (_incomingVideoChannel) {
            _incomingVideoChannel->addSink(sink);
        }
        if (_incomingScreencastChannel) {
            _incomingScreencastChannel->addSink(sink);
        }
    }

    void setAudioInputDevice(std::string id) {

    }

    void setAudioOutputDevice(std::string id) {

    }

    void setIsLowBatteryLevel(bool isLowBatteryLevel) {
        if (_isBatteryLow != isLowBatteryLevel) {
            _isBatteryLow = isLowBatteryLevel;
            sendMediaState();
        }
    }

    void stop(std::function<void(FinalState)> completion) {
        FinalState finalState;
        
        json11::Json::object statsLog;
        
        for (int i = (int)_networkStateLogRecords.size() - 1; i >= 1; i--) {
            // coalesce events within 5ms
            if (_networkStateLogRecords[i].timestamp - _networkStateLogRecords[i - 1].timestamp < 5) {
                _networkStateLogRecords.erase(_networkStateLogRecords.begin() + i - 1);
            }
        }
        
        json11::Json::array jsonNetworkStateLogRecords;
        int64_t baseTimestamp = 0;
        for (const auto &record : _networkStateLogRecords) {
            json11::Json::object jsonRecord;
            
            std::ostringstream timestampString;
            
            if (baseTimestamp == 0) {
                baseTimestamp = record.timestamp;
            }
            timestampString << (record.timestamp - baseTimestamp);
            
            jsonRecord.insert(std::make_pair("t", json11::Json(timestampString.str())));
            jsonRecord.insert(std::make_pair("c", json11::Json(record.record.isConnected ? 1 : 0)));
            if (record.record.route) {
                jsonRecord.insert(std::make_pair("local", json11::Json(record.record.route->localDescription)));
                jsonRecord.insert(std::make_pair("remote", json11::Json(record.record.route->remoteDescription)));
            }
            if (record.record.connection) {
                json11::Json::object jsonConnection;
                
                auto serializeCandidate = [](NativeNetworkingImpl::ConnectionDescription::CandidateDescription const &candidate) -> json11::Json::object {
                    json11::Json::object jsonCandidate;
                    
                    jsonCandidate.insert(std::make_pair("type", json11::Json(candidate.type)));
                    jsonCandidate.insert(std::make_pair("protocol", json11::Json(candidate.protocol)));
                    jsonCandidate.insert(std::make_pair("address", json11::Json(candidate.address)));
                    
                    return jsonCandidate;
                };
                
                jsonConnection.insert(std::make_pair("local", serializeCandidate(record.record.connection->local)));
                jsonConnection.insert(std::make_pair("remote", serializeCandidate(record.record.connection->remote)));
                
                jsonRecord.insert(std::make_pair("network", std::move(jsonConnection)));
            }
            if (record.record.isFailed) {
                jsonRecord.insert(std::make_pair("failed", json11::Json(1)));
            }
            
            jsonNetworkStateLogRecords.push_back(std::move(jsonRecord));
        }
        statsLog.insert(std::make_pair("network", std::move(jsonNetworkStateLogRecords)));
        
        json11::Json::array jsonNetworkBitrateLogRecords;
        for (const auto &record : _networkBitrateLogRecords) {
            json11::Json::object jsonRecord;
            
            jsonRecord.insert(std::make_pair("b", json11::Json(record.record.bitrate)));
            
            jsonNetworkBitrateLogRecords.push_back(std::move(jsonRecord));
        }
        statsLog.insert(std::make_pair("bitrate", std::move(jsonNetworkBitrateLogRecords)));
        
        auto jsonStatsLog = json11::Json(std::move(statsLog));
        
        if (!_statsLogPath.data.empty()) {
            std::ofstream file;
            file.open(_statsLogPath.data);
            
            file << jsonStatsLog.dump();
            
            file.close();
        }
        
        completion(finalState);
    }

    void adjustBitratePreferences(bool resetStartBitrate) {
        if (_outgoingAudioChannel) {
            _outgoingAudioChannel->setMaxBitrate(32 * 1024);
        }
        if (_outgoingVideoChannel) {
            _outgoingVideoChannel->setMaxBitrate(1000 * 1024);
        }
    }

private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule() {
        const auto create = [&](webrtc::AudioDeviceModule::AudioLayer layer) {
#ifdef WEBRTC_IOS
            return rtc::make_ref_counted<webrtc::tgcalls_ios_adm::AudioDeviceModuleIOS>(false, false, 1);
#else
            return webrtc::AudioDeviceModule::Create(
                layer,
                _taskQueueFactory.get());
#endif
        };
        const auto check = [&](const rtc::scoped_refptr<webrtc::AudioDeviceModule> &result) {
            return (result && result->Init() == 0) ? result : nullptr;
        };
        if (_createAudioDeviceModule) {
            if (const auto result = check(_createAudioDeviceModule(_taskQueueFactory.get()))) {
                return result;
            }
        }
        return check(create(webrtc::AudioDeviceModule::kPlatformDefaultAudio));
    }

private:
    std::shared_ptr<Threads> _threads;
    std::vector<RtcServer> _rtcServers;
    std::unique_ptr<Proxy> _proxy;
    bool _enableP2P = false;
    EncryptionKey _encryptionKey;
    std::function<void(State)> _stateUpdated;
    std::function<void(int)> _signalBarsUpdated;
    std::function<void(float)> _audioLevelUpdated;
    std::function<void(bool)> _remoteBatteryLevelIsLowUpdated;
    std::function<void(AudioState, VideoState)> _remoteMediaStateUpdated;
    std::function<void(float)> _remotePrefferedAspectRatioUpdated;
    std::function<void(const std::vector<uint8_t> &)> _signalingDataEmitted;
    std::function<rtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory*)> _createAudioDeviceModule;
    FilePath _statsLogPath;

    std::unique_ptr<SignalingEncryption> _signalingEncryption;
    
    int64_t _startTimestamp = 0;
    
    absl::optional<NetworkStateLogRecord> _currentNetworkStateLogRecord;
    std::vector<StateLogRecord<NetworkStateLogRecord>> _networkStateLogRecords;
    std::vector<StateLogRecord<NetworkBitrateLogRecord>> _networkBitrateLogRecords;
    
    absl::optional<NativeNetworkingImpl::State> _networkState;

    bool _handshakeCompleted = false;
    std::vector<cricket::Candidate> _pendingIceCandidates;
    bool _isDataChannelOpen = false;

    std::unique_ptr<webrtc::RtcEventLogNull> _eventLog;
    std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
    std::unique_ptr<cricket::MediaEngineInterface> _mediaEngine;
    std::unique_ptr<webrtc::Call> _call;
    webrtc::FieldTrialBasedConfig _fieldTrials;
    webrtc::LocalAudioSinkAdapter _audioSource;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;

    std::unique_ptr<rtc::UniqueRandomIdGenerator> _uniqueRandomIdGenerator;
    webrtc::RtpTransport *_rtpTransport = nullptr;
    std::unique_ptr<cricket::ChannelManager> _channelManager;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;
    
    std::unique_ptr<ContentNegotiationContext> _contentNegotiationContext;

    std::shared_ptr<ThreadLocalObject<NativeNetworkingImpl>> _networking;

    absl::optional<std::string> _outgoingAudioChannelId;
    std::unique_ptr<OutgoingAudioChannel> _outgoingAudioChannel;
    bool _isMicrophoneMuted = false;

    std::vector<webrtc::SdpVideoFormat> _availableVideoFormats;

    absl::optional<std::string> _outgoingVideoChannelId;
    std::shared_ptr<OutgoingVideoChannel> _outgoingVideoChannel;
    absl::optional<std::string> _outgoingScreencastChannelId;
    std::shared_ptr<OutgoingVideoChannel> _outgoingScreencastChannel;

    bool _isBatteryLow = false;

    std::unique_ptr<IncomingV2AudioChannel> _incomingAudioChannel;
    std::unique_ptr<IncomingV2VideoChannel> _incomingVideoChannel;
    std::unique_ptr<IncomingV2VideoChannel> _incomingScreencastChannel;

    std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _currentSink;

    std::shared_ptr<VideoCaptureInterface> _videoCapture;
    std::shared_ptr<VideoCaptureInterface> _screencastCapture;
    std::shared_ptr<PlatformContext> _platformContext;
};

InstanceV2Impl::InstanceV2Impl(Descriptor &&descriptor) {
    if (descriptor.config.logPath.data.size() != 0) {
        _logSink = std::make_unique<LogSinkImpl>(descriptor.config.logPath);
    }
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(false);
    if (_logSink) {
        rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
    }

    _threads = StaticThreads::getThreads();
    _internal.reset(new ThreadLocalObject<InstanceV2ImplInternal>(_threads->getMediaThread(), [descriptor = std::move(descriptor), threads = _threads]() mutable {
        return new InstanceV2ImplInternal(std::move(descriptor), threads);
    }));
    _internal->perform(RTC_FROM_HERE, [](InstanceV2ImplInternal *internal) {
        internal->start();
    });
}

InstanceV2Impl::~InstanceV2Impl() {
    rtc::LogMessage::RemoveLogToStream(_logSink.get());
}

void InstanceV2Impl::receiveSignalingData(const std::vector<uint8_t> &data) {
    _internal->perform(RTC_FROM_HERE, [data](InstanceV2ImplInternal *internal) {
        internal->receiveSignalingData(data);
    });
}

void InstanceV2Impl::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    _internal->perform(RTC_FROM_HERE, [videoCapture](InstanceV2ImplInternal *internal) {
        internal->setVideoCapture(videoCapture);
    });
}

void InstanceV2Impl::setRequestedVideoAspect(float aspect) {
    _internal->perform(RTC_FROM_HERE, [aspect](InstanceV2ImplInternal *internal) {
        internal->setRequestedVideoAspect(aspect);
    });
}

void InstanceV2Impl::setNetworkType(NetworkType networkType) {
    _internal->perform(RTC_FROM_HERE, [networkType](InstanceV2ImplInternal *internal) {
        internal->setNetworkType(networkType);
    });
}

void InstanceV2Impl::setMuteMicrophone(bool muteMicrophone) {
    _internal->perform(RTC_FROM_HERE, [muteMicrophone](InstanceV2ImplInternal *internal) {
        internal->setMuteMicrophone(muteMicrophone);
    });
}

void InstanceV2Impl::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _internal->perform(RTC_FROM_HERE, [sink](InstanceV2ImplInternal *internal) {
        internal->setIncomingVideoOutput(sink);
    });
}

void InstanceV2Impl::setAudioInputDevice(std::string id) {
    _internal->perform(RTC_FROM_HERE, [id](InstanceV2ImplInternal *internal) {
        internal->setAudioInputDevice(id);
    });
}

void InstanceV2Impl::setAudioOutputDevice(std::string id) {
    _internal->perform(RTC_FROM_HERE, [id](InstanceV2ImplInternal *internal) {
        internal->setAudioOutputDevice(id);
    });
}

void InstanceV2Impl::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _internal->perform(RTC_FROM_HERE, [isLowBatteryLevel](InstanceV2ImplInternal *internal) {
        internal->setIsLowBatteryLevel(isLowBatteryLevel);
    });
}

void InstanceV2Impl::setInputVolume(float level) {
}

void InstanceV2Impl::setOutputVolume(float level) {
}

void InstanceV2Impl::setAudioOutputDuckingEnabled(bool enabled) {
}

void InstanceV2Impl::setAudioOutputGainControlEnabled(bool enabled) {
}

void InstanceV2Impl::setEchoCancellationStrength(int strength) {
}

std::vector<std::string> InstanceV2Impl::GetVersions() {
    std::vector<std::string> result;
    result.push_back("4.0.1");
    return result;
}

int InstanceV2Impl::GetConnectionMaxLayer() {
    return 92;
}

std::string InstanceV2Impl::getLastError() {
    return "";
}

std::string InstanceV2Impl::getDebugInfo() {
    return "";
}

int64_t InstanceV2Impl::getPreferredRelayId() {
    return 0;
}

TrafficStats InstanceV2Impl::getTrafficStats() {
    return {};
}

PersistentState InstanceV2Impl::getPersistentState() {
    return {};
}

void InstanceV2Impl::stop(std::function<void(FinalState)> completion) {
    std::string debugLog;
    if (_logSink) {
        debugLog = _logSink->result();
    }
    _internal->perform(RTC_FROM_HERE, [completion, debugLog = std::move(debugLog)](InstanceV2ImplInternal *internal) mutable {
        internal->stop([completion, debugLog = std::move(debugLog)](FinalState finalState) mutable {
            finalState.debugLog = debugLog;
            completion(finalState);
        });
    });
}

template <>
bool Register<InstanceV2Impl>() {
    return Meta::RegisterOne<InstanceV2Impl>();
}

} // namespace tgcalls
