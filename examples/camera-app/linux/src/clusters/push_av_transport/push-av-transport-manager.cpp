/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/push-av-stream-transport-server/push-av-stream-transport-server.h>
#include <fstream>
#include <iostream>
#include <lib/support/logging/CHIPLogging.h>
#include <push-av-transport-manager.h>
#include <pushav-clip-recorder.h>
#include <pushav-transport.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::DataModel;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::PushAvStreamTransport;
using chip::Protocols::InteractionModel::Status;
using namespace Camera;

// TODO: ConfigureRecorderSettings improvements needed
// 1. Implement proper storage path resolution instead of fixed "./clips/"
// 2. Calculate frame durations dynamically from stream properties

const char * GetAudioCodecName(int codecId)
{
    switch (codecId)
    {
    case AV_CODEC_ID_OPUS:
        return "OPUS";
    default:
        return "Unknown";
    }
}

const char * GetVideoCodecName(int codecId)
{
    switch (codecId)
    {
    case AV_CODEC_ID_H264:
        return "H.264";
    default:
        return "Unknown";
    }
}

void PrintTransportSettings1(PushAVTransport * transport)
{
    const auto & clipInfo  = transport->clipInfo;
    const auto & audioInfo = transport->audioInfo;
    const auto & videoInfo = transport->videoInfo;

    ChipLogProgress(Camera, "=== Clip Configuration ===");
    ChipLogProgress(Camera, "Has Audio: %s", clipInfo.mHasAudio ? "true" : "false");
    ChipLogProgress(Camera, "Has Video: %s", clipInfo.mHasVideo ? "true" : "false");
    ChipLogProgress(Camera, "Initial Duration: %d sec", clipInfo.mInitialDuration);
    ChipLogProgress(Camera, "Augmentation Duration: %d sec", clipInfo.mAugmentationDuration);
    ChipLogProgress(Camera, "Max Clip Duration: %d sec", clipInfo.mMaxClipDuration);
    ChipLogProgress(Camera, "Chunk Duration: %d sec", clipInfo.mChunkDuration);
    ChipLogProgress(Camera, "URL: %s", clipInfo.mUrl.c_str());
    ChipLogProgress(Camera, "Trigger Type: %d", clipInfo.mTriggerType);
    ChipLogProgress(Camera, "recorder id %s", clipInfo.mRecorderId.c_str());
    ChipLogProgress(Camera, "Output Path: %s", clipInfo.mOutputPath.c_str());
    ChipLogProgress(Camera, "Input Time Base: %d/%d", clipInfo.mInputTimeBase.num, clipInfo.mInputTimeBase.den);

    ChipLogProgress(Camera, "=== Audio Configuration ===");
    ChipLogProgress(Camera, "Codec: %s", GetAudioCodecName(audioInfo.mAudioCodecId));
    ChipLogProgress(Camera, "Channels: %d", audioInfo.mChannels);
    ChipLogProgress(Camera, "Sample Rate: %d Hz", audioInfo.mSampleRate);
    ChipLogProgress(Camera, "Bit Rate: %d bps", audioInfo.mBitRate);
    ChipLogProgress(Camera, "Audio Time Base: %d/%d", audioInfo.mAudioTimeBase.num, audioInfo.mAudioTimeBase.den);
    ChipLogProgress(Camera, "Frame Duration: %d samples", audioInfo.mAudioFrameDuration);

    ChipLogProgress(Camera, "=== Video Configuration ===");
    ChipLogProgress(Camera, "Codec: %s", GetVideoCodecName(videoInfo.mVideoCodecId));
    ChipLogProgress(Camera, "Resolution: %dx%d", videoInfo.mWidth, videoInfo.mHeight);
    ChipLogProgress(Camera, "Frame Rate: %d fps", videoInfo.mFrameRate);
    ChipLogProgress(Camera, "Video Time Base: %d/%d", videoInfo.mVideoTimeBase.num, videoInfo.mVideoTimeBase.den);
    ChipLogProgress(Camera, "Frame Duration: %d ticks", videoInfo.mVideoFrameDuration);
    ChipLogProgress(Camera, "Bit Rate: %d bps", videoInfo.mBitRate);
}

void PushAvStreamTransportManager::ConfigureRecorderSettings(PushAVTransport * transport,
                                                             const TransportOptionsDecodeableStruct & transportOptions,
                                                             TransportConfigurationStruct & outTransporConfiguration)
{

    PushAVClipRecorder::ClipInfoStruct clipInfo;
    PushAVClipRecorder::AudioInfoStruct audioInfo;
    PushAVClipRecorder::VideoInfoStruct videoInfo;

    clipInfo.mHasAudio = true;
    clipInfo.mHasVideo = true;

    if (outTransporConfiguration.transportOptions.HasValue())
    {
        clipInfo.mUrl = outTransporConfiguration.transportOptions.Value().url.data();
        if (0 /* outTransporConfiguration.transportOptions.Value().triggerOptions.motionTimeControl.HasValue()*/)
        {
            clipInfo.mInitialDuration =
                outTransporConfiguration.transportOptions.Value().triggerOptions.motionTimeControl.Value().initialDuration;
            clipInfo.mAugmentationDuration =
                outTransporConfiguration.transportOptions.Value().triggerOptions.motionTimeControl.Value().augmentationDuration;
            clipInfo.mMaxClipDuration =
                outTransporConfiguration.transportOptions.Value().triggerOptions.motionTimeControl.Value().maxDuration;
            clipInfo.mBlindDuration =
                outTransporConfiguration.transportOptions.Value().triggerOptions.motionTimeControl.Value().blindDuration;
        }
        else
        {
            clipInfo.mInitialDuration      = 30;
            clipInfo.mAugmentationDuration = 20;
            clipInfo.mBlindDuration        = 5;
            clipInfo.mMaxClipDuration      = 100;
        }
        if (0 /*outTransporConfiguration.transportOptions.Value().containerOptions.CMAFContainerOptions.HasValue()*/)
        {
            clipInfo.mChunkDuration =
                outTransporConfiguration.transportOptions.Value().containerOptions.CMAFContainerOptions.Value().chunkDuration;
        }
        else
        {
            clipInfo.mChunkDuration = 5;
        }
    }
    else
    {
        clipInfo.mInitialDuration      = 30;
        clipInfo.mAugmentationDuration = 20;
        clipInfo.mBlindDuration        = 5;
        clipInfo.mMaxClipDuration      = 100;
        clipInfo.mChunkDuration        = 5;
        clipInfo.mUrl                  = "https://localhost:1234/streams/1/";
    }

    clipInfo.mUrl           = "https://localhost:1234/streams/1/";
    int triggerType         = static_cast<int>(transportOptions.triggerOptions.triggerType);
    clipInfo.mTriggerType   = triggerType;
    clipInfo.mClipId        = 0;
    clipInfo.mOutputPath    = "./clips/";
    clipInfo.mInputTimeBase = { 1, 1000000 };

    uint8_t audioCodec  = static_cast<uint8_t>(mAudioStreamParams.audioCodec);
    audioInfo.mChannels = 1; // mAudioStreamParams.channelCount;

    if (audioCodec == 0)
    {
        audioInfo.mAudioCodecId       = AV_CODEC_ID_OPUS;
        audioInfo.mAudioTimeBase      = { 1, 48000 };
        audioInfo.mAudioFrameDuration = 19200;
    }
    else if (audioCodec == 2)
    {
        ChipLogError(Camera, "Unknown Audio codec")
    }
    else
    {
        ChipLogError(Camera, "Unsupported Audio codec");
    }

    audioInfo.mSampleRate       = mAudioStreamParams.sampleRate;
    audioInfo.mBitRate          = mAudioStreamParams.bitRate;
    audioInfo.mAudioPts         = 0;
    audioInfo.mAudioDts         = 0;
    audioInfo.mAudioStreamIndex = -1;

    int8_t VideoCodec = static_cast<uint8_t>(mVideoStreamParams.videoCodec);
    if (VideoCodec == 0)
    {
        videoInfo.mVideoCodecId  = AV_CODEC_ID_H264;
        videoInfo.mVideoTimeBase = { 1, 90000 };
    }
    else if (VideoCodec == 4)
    {
        ChipLogError(Camera, "Unknown Video codec")
    }
    else
    {
        ChipLogError(Camera, "Unsupported Video codec");
    }
    videoInfo.mVideoPts  = 0;
    videoInfo.mVideoDts  = 0;
    videoInfo.mWidth     = mVideoStreamParams.maxResolution.width;
    videoInfo.mHeight    = mVideoStreamParams.maxResolution.height;
    videoInfo.mFrameRate = mVideoStreamParams.minFrameRate;

    videoInfo.mVideoFrameDuration = 900000 / videoInfo.mFrameRate;
    videoInfo.mVideoStreamIndex   = -1;
    videoInfo.mBitRate            = mVideoStreamParams.minBitRate;

    transport->clipInfo  = clipInfo;
    transport->audioInfo = audioInfo;
    transport->videoInfo = videoInfo;
    PrintTransportSettings1(transport);
    ChipLogProgress(Camera, "PushAvStreamTransportManager, Configure Recorder Settings done !!!");
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::AllocatePushTransport(const TransportOptionsDecodeableStruct & transportOptions,
                                                    TransportConfigurationStruct & outTransporConfiguration)
{
    uint16_t connectionID = outTransporConfiguration.connectionID;

    mTransportOptionsMap[connectionID] = transportOptions;
    mTransportConfigMap[connectionID]  = outTransporConfiguration;

    ChipLogProgress(Camera, "PushAvStreamTransportManager, Create PushAV Transport for Connection: [%u]", connectionID);
    mTransportMap[connectionID] = std::move(
        std::make_unique<PushAVTransport>(connectionID, transportOptions.url.data(), transportOptions.triggerOptions.triggerType));

    mMediaController->RegisterTransport(mTransportMap[connectionID].get(), transportOptions.videoStreamID.Value().Value(),
                                        transportOptions.audioStreamID.Value().Value());

    ConfigureRecorderSettings(mTransportMap[connectionID].get(), transportOptions, outTransporConfiguration);

    return Status::Success;
}

PushAvStreamTransportManager::~PushAvStreamTransportManager()
{
    // Unregister all transports from Media Controller before deleting them. This will ensure that any ongoing streams are stopped.
    if (mMediaController != nullptr)
    {
        for (auto & kv : mTransportMap)
        {
            mMediaController->UnregisterTransport(kv.second.get());
        }
    }
    mTransportMap.clear();
    mTransportOptionsMap.clear();
    mTransportConfigMap.clear();
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::DeallocatePushTransport(const uint16_t connectionID)
{
    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return Status::NotFound;
    }

    mMediaController->UnregisterTransport(mTransportMap[connectionID].get());
    mTransportMap.erase(connectionID);
    mTransportOptionsMap.erase(connectionID);
    mTransportConfigMap.erase(connectionID);

    return Status::Success;
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::ModifyPushTransport(const uint16_t connectionID,
                                                  const TransportOptionsDecodeableStruct & transportOptions)
{
    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return Status::NotFound;
    }

    mTransportOptionsMap[connectionID] = transportOptions;

    ChipLogProgress(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);

    return Status::Success;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::SetTransportStatus(const std::vector<uint16_t> & connectionIDList,
                                                                                     TransportStatusEnum transportStatus)
{
    if (connectionIDList.empty())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, connectionIDList is empty");
        return Status::Failure;
    }

    if (transportStatus == TransportStatusEnum::kUnknownEnumValue)
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, Invalid TransportStatus, transportStatus: [%u]",
                     (uint16_t) transportStatus);
        return Status::Failure;
    }

    for (uint16_t connectionID : connectionIDList)
    {
        if (mTransportMap.find(connectionID) == mTransportMap.end())
        {
            ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
            continue;
        }
        mTransportMap[connectionID]->setTransportStatus(transportStatus);
    }

    return Status::Success;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::ManuallyTriggerTransport(
    const uint16_t connectionID, TriggerActivationReasonEnum activationReason,
    const Optional<Structs::TransportMotionTriggerTimeControlStruct::DecodableType> & timeControl)
{
    if (activationReason == TriggerActivationReasonEnum::kUnknownEnumValue)
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, Manual Trigger failed for connection [%u], reason: [%u]", connectionID,
                     (uint16_t) activationReason);
        return Status::Failure;
    }

    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return Status::NotFound;
    }

    ChipLogProgress(Camera, "PushAvStreamTransportManager, Trigger PushAV Transport for Connection: [%u]", connectionID);
    mTransportMap[connectionID]->TriggerTransport(activationReason);

    return Status::Success;
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::FindTransport(const Optional<DataModel::Nullable<uint16_t>> & connectionID,
                                            DataModel::List<const TransportConfigurationStruct> & outTransportConfigurations)
{
    if (!connectionID.HasValue())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, connectionID not available");
        return Status::Failure;
    }

    std::vector<TransportConfigurationStruct> configList;

    if (connectionID.Value().IsNull())
    {
        for (auto & it : mTransportConfigMap)
        {
            configList.push_back(it.second);
        }
    }
    else
    {
        for (auto & it : mTransportConfigMap)
        {
            if (connectionID.Value().Value() == it.first)
            {
                configList.push_back(it.second);
            }
        }
    }

    outTransportConfigurations = DataModel::List<const TransportConfigurationStruct>(configList.data(), configList.size());

    return Status::Success;
}

CHIP_ERROR
PushAvStreamTransportManager::ValidateStreamUsage(StreamUsageEnum streamUsage,
                                                  const Optional<DataModel::Nullable<uint16_t>> & videoStreamId,
                                                  const Optional<DataModel::Nullable<uint16_t>> & audioStreamId)
{
    // TODO: Validates the requested stream usage against the camera's resource management and stream priority policies.
    return CHIP_NO_ERROR;
}

void PushAvStreamTransportManager::OnAttributeChanged(AttributeId attributeId)
{
    ChipLogProgress(Zcl, "Attribute changed for AttributeId = " ChipLogFormatMEI, ChipLogValueMEI(attributeId));
}

void PushAvStreamTransportManager::Init(MediaController * mediaController, AudioStreamStruct aAudioStreamParams,
                                        VideoStreamStruct aVideoStreamParams)
{
    mMediaController   = mediaController;
    mVideoStreamParams = aVideoStreamParams;
    mAudioStreamParams = aAudioStreamParams;
    return;
}

CHIP_ERROR PushAvStreamTransportManager::LoadCurrentConnections(std::vector<TransportConfigurationStruct> & currentConnections)
{
    ChipLogError(Zcl, "Push AV Current Connections loaded");

    return CHIP_NO_ERROR;
}

CHIP_ERROR
PushAvStreamTransportManager::PersistentAttributesLoadedCallback()
{
    ChipLogError(Zcl, "Persistent attributes loaded");

    return CHIP_NO_ERROR;
}
