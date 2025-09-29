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

#include "push-av-stream-manager.h"

#include <algorithm>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <fstream>
#include <iostream>
#include <lib/support/logging/CHIPLogging.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::DataModel;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::PushAvStreamTransport;
using chip::Protocols::InteractionModel::Status;

PushAvStreamTransportManager::~PushAvStreamTransportManager()
{
    // Unregister all transports from Media Controller before deleting them. This will ensure that any ongoing streams are
    // stopped.
    if (mMediaController != nullptr)
    {
        for (auto & kv : mTransportMap)
        {
            mMediaController->UnregisterTransport(kv.second.get());
        }
    }
    mTransportMap.clear();
    mTransportOptionsMap.clear();
}

void PushAvStreamTransportManager::Init()
{
    ChipLogProgress(Zcl, "Push AV Stream Transport Initialized");
    return;
}

void PushAvStreamTransportManager::SetMediaController(MediaController * mediaController)
{
    mMediaController = mediaController;
}

void PushAvStreamTransportManager::SetCameraDevice(CameraDeviceInterface * aCameraDevice)
{
    mCameraDevice = aCameraDevice;
}

void PushAvStreamTransportManager::SetPushAvStreamTransportServer(PushAvStreamTransportServer * server)
{
    mPushAvStreamTransportServer = server;
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::AllocatePushTransport(const TransportOptionsStruct & transportOptions, const uint16_t connectionID)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for AllocatePushTransport");
        return Status::Failure;
    }
    mTransportOptionsMap[connectionID] = transportOptions;

    ChipLogProgress(Camera, "PushAvStreamTransportManager, Create PushAV Transport for Connection: [%u]", connectionID);
    mTransportMap[connectionID] =
        std::make_unique<PushAVTransport>(transportOptions, connectionID, mAudioStreamParams, mVideoStreamParams);

    mTransportMap[connectionID]->SetPushAvStreamTransportServer(mPushAvStreamTransportServer);

    if (mMediaController == nullptr)
    {
        ChipLogError(Camera, "PushAvStreamTransportManager: MediaController is not set");
        mTransportMap.erase(connectionID);
        return Status::NotFound;
    }

    mMediaController->RegisterTransport(mTransportMap[connectionID].get(), transportOptions.videoStreamID.Value().Value(),
                                        transportOptions.audioStreamID.Value().Value());
    mMediaController->SetPreRollLength(mTransportMap[connectionID].get(), mTransportMap[connectionID].get()->GetPreRollLength());

    uint32_t newTransportBandwidthbps = 0;
    GetBandwidthForStreams(transportOptions.videoStreamID, transportOptions.audioStreamID, newTransportBandwidthbps);

    mTransportMap[connectionID].get()->SetCurrentlyUsedBandwidthbps(newTransportBandwidthbps);
    mTotalUsedBandwidthbps += newTransportBandwidthbps;
    ChipLogDetail(Camera,
                  "AllocatePushTransport: Transport for connection %u allocated successfully. "
                  "New transport bandwidth: %u bps. Total used bandwidth: %u bps.",
                  connectionID, newTransportBandwidthbps, mTotalUsedBandwidthbps);

    if (transportOptions.triggerOptions.triggerType == TransportTriggerTypeEnum::kMotion &&
        transportOptions.triggerOptions.motionZones.HasValue())
    {
        std::vector<std::pair<chip::app::DataModel::Nullable<uint16_t>, uint8_t>> zoneSensitivityList;

        auto motionZones = transportOptions.triggerOptions.motionZones.Value().Value();
        for (const auto & zoneOption : motionZones)
        {
            if (zoneOption.sensitivity.HasValue())
            {
                zoneSensitivityList.push_back({ zoneOption.zone, zoneOption.sensitivity.Value() });
            }
            else
            {
                zoneSensitivityList.push_back(
                    { zoneOption.zone, transportOptions.triggerOptions.motionSensitivity.Value().Value() });
            }
        }

        if (!zoneSensitivityList.empty())
        {
            mTransportMap[connectionID].get()->SetZoneSensitivityList(zoneSensitivityList);
        }
    }

#ifndef TLS_CLUSTER_NOT_ENABLED
    ChipLogDetail(Camera, "PushAvStreamTransportManager: TLS Cluster enabled, using default certs");
    mTransportMap[connectionID].get()->SetTLSCert(mBufferRootCert, mBufferClientCert, mBufferClientCertKey,
                                                  mBufferIntermediateCerts);
#else
    // TODO: The else block is for testing purpose. It should be removed once the TLS cluster integration is stable.
    mTransportMap[connectionID].get()->SetTLSCertPath("/tmp/pavstest/certs/server/root.pem", "/tmp/pavstest/certs/device/dev.pem",
                                                      "/tmp/pavstest/certs/device/dev.key");
#endif
    return Status::Success;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::DeallocatePushTransport(const uint16_t connectionID)
{
    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return Status::NotFound;
    }
    mTotalUsedBandwidthbps -= mTransportMap[connectionID].get()->GetCurrentlyUsedBandwidthbps();
    mMediaController->UnregisterTransport(mTransportMap[connectionID].get());
    mTransportMap.erase(connectionID);
    mTransportOptionsMap.erase(connectionID);

    return Status::Success;
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::ModifyPushTransport(const uint16_t connectionID, const TransportOptionsStorage transportOptions)
{
    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return Status::NotFound;
    }

    uint32_t newTransportBandwidthbps = 0;
    GetBandwidthForStreams(transportOptions.videoStreamID, transportOptions.audioStreamID, newTransportBandwidthbps);

    mTotalUsedBandwidthbps -= mTransportMap[connectionID].get()->GetCurrentlyUsedBandwidthbps();

    mTransportMap[connectionID].get()->SetCurrentlyUsedBandwidthbps(newTransportBandwidthbps);
    mTotalUsedBandwidthbps += newTransportBandwidthbps;

    ChipLogDetail(Camera,
                  "ModifyPushTransport: Transport for connection %u allocated successfully. "
                  "New transport bandwidth: %u bps. Total used bandwidth: %u bps.",
                  connectionID, newTransportBandwidthbps, mTotalUsedBandwidthbps);

    mTransportOptionsMap[connectionID] = transportOptions;
    mTransportMap[connectionID].get()->ModifyPushTransport(transportOptions);
    ChipLogProgress(Camera, "PushAvStreamTransportManager, success to modify Connection :[%u]", connectionID);

    return Status::Success;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::SetTransportStatus(const std::vector<uint16_t> connectionIDList,
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

    if (transportStatus == TransportStatusEnum::kActive)
    {
        auto & avsmController = mCameraDevice->GetCameraAVStreamMgmtController();
        bool isActive;
        CHIP_ERROR status = avsmController.IsPrivacyModeActive(isActive);
        if (status != CHIP_NO_ERROR)
        {
            ChipLogError(Camera,
                         "PushAvStreamTransportManager, Failed to retrieve Privacy Mode Status from AVStreamMgmtController.");
            return Status::Failure;
        }

        if (isActive)
        {
            ChipLogError(Camera, "PushAvStreamTransportManager, Cannot set transport status to Active as privacy mode is enabled.");
            return Status::InvalidInState;
        }
    }

    for (uint16_t connectionID : connectionIDList)
    {
        if (mTransportMap.find(connectionID) == mTransportMap.end())
        {
            ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
            continue;
        }
        mTransportMap[connectionID]->SetTransportStatus(transportStatus);
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
    if (timeControl.HasValue())
    {
        mTransportMap[connectionID]->ConfigureRecorderTimeSetting(timeControl.Value());
    }
    mTransportMap[connectionID]->TriggerTransport(activationReason);

    return Status::Success;
}

void PushAvStreamTransportManager::GetBandwidthForStreams(const Optional<DataModel::Nullable<uint16_t>> & videoStreamId,
                                                          const Optional<DataModel::Nullable<uint16_t>> & audioStreamId,
                                                          uint32_t & outBandwidthbps)
{
    mCameraDevice->GetCameraAVStreamMgmtDelegate().GetBandwidthForStreams(videoStreamId, audioStreamId, outBandwidthbps);
    return;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::GetVideoStreamIdForStreams(StreamUsageEnum streamUsage,
                                                                                              uint16_t & videoStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for GetVideoStreamIdForStreams");
        return Status::Failure;
    }
    return mCameraDevice->GetCameraAVStreamMgmtDelegate().GetVideoStreamIdForStreams(streamUsage, videoStreamId);
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::GetAudioStreamIdForStreams(StreamUsageEnum streamUsage,
                                                                                              uint16_t & audioStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for GetAudioStreamIdForStreams");
        return Status::Failure;
    }
    return mCameraDevice->GetCameraAVStreamMgmtDelegate().GetAudioStreamIdForStreams(streamUsage, audioStreamId);
}

Protocols::InteractionModel::Status
PushAvStreamTransportManager::ValidateBandwidthLimit(StreamUsageEnum streamUsage,
                                                     const Optional<DataModel::Nullable<uint16_t>> & videoStreamId,
                                                     const Optional<DataModel::Nullable<uint16_t>> & audioStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for ValidateBandwidthLimit");
        return Status::Failure;
    }

    uint32_t newStreamBandwidthbps = 0;
    GetBandwidthForStreams(videoStreamId, audioStreamId, newStreamBandwidthbps);
    uint32_t maxNetworkBandwidthbps = mCameraDevice->GetCameraHALInterface().GetMaxNetworkBandwidth();

    uint32_t projectedTotalBandwidthbps = mTotalUsedBandwidthbps + newStreamBandwidthbps;

    ChipLogProgress(Camera,
                    "ValidateBandwidthLimit: For streamUsage %u. New stream bandwidth: %u bps. "
                    "Currently used bandwidth: %u bps. Projected total: %u bps. Max allowed: %u bps.",
                    static_cast<uint16_t>(streamUsage), newStreamBandwidthbps, mTotalUsedBandwidthbps, projectedTotalBandwidthbps,
                    maxNetworkBandwidthbps);

    if (projectedTotalBandwidthbps > maxNetworkBandwidthbps)
    {
        ChipLogError(Camera,
                     "ValidateBandwidthLimit: ResourceExhausted for streamUsage %u. "
                     "Projected total bandwidth (%u bps) would exceed maximum network bandwidth (%u bps). "
                     "New stream requires %u bps, currently %u bps is in use.",
                     static_cast<uint16_t>(streamUsage), projectedTotalBandwidthbps, maxNetworkBandwidthbps, newStreamBandwidthbps,
                     mTotalUsedBandwidthbps);
        return Status::ResourceExhausted;
    }

    ChipLogProgress(Camera,
                    "ValidateBandwidthLimit: Success for streamUsage %u. "
                    "Allocating this stream would keep bandwidth usage within limits.",
                    static_cast<uint16_t>(streamUsage));
    return Status::Success;
}

bool PushAvStreamTransportManager::ValidateStreamUsage(StreamUsageEnum streamUsage,
                                                       const Optional<DataModel::Nullable<uint16_t>> & videoStreamId,
                                                       const Optional<DataModel::Nullable<uint16_t>> & audioStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for ValidateStreamUsage");
        return false;
    }

    auto & avsmController                                     = mCameraDevice->GetCameraAVStreamMgmtController();
    Optional<DataModel::Nullable<uint16_t>> videoStreamIdCopy = videoStreamId;
    Optional<DataModel::Nullable<uint16_t>> audioStreamIdCopy = audioStreamId;

    CHIP_ERROR err = avsmController.ValidateStreamUsage(streamUsage, videoStreamIdCopy, audioStreamIdCopy);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "ValidateStreamUsage failed for streamUsage %u: %" CHIP_ERROR_FORMAT,
                     static_cast<uint16_t>(streamUsage), err.Format());
        return false;
    }

    ChipLogProgress(Camera, "ValidateStreamUsage succeeded for streamUsage %u", static_cast<uint16_t>(streamUsage));
    return true;
}

bool PushAvStreamTransportManager::ValidateSegmentDuration(uint16_t segmentDuration,
                                                           const Optional<DataModel::Nullable<uint16_t>> & videoStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized for ValidateSegmentDuration");
        return false;
    }

    if (!videoStreamId.HasValue() || videoStreamId.Value().IsNull())
    {
        ChipLogError(Camera, "ValidateSegmentDuration failed: VideoStreamID not provided or is null");
        return false;
    }

    uint16_t targetVideoStreamId = videoStreamId.Value().Value();

    if (segmentDuration < 500 || segmentDuration > 65500)
    {
        ChipLogError(Camera, "ValidateSegmentDuration failed: Segment duration %ums must be between 500ms and 65500ms",
                     segmentDuration);
        return false;
    }

    auto & avsmController = mCameraDevice->GetCameraAVStreamMgmtController();

    CHIP_ERROR err = avsmController.ValidateVideoStreamID(targetVideoStreamId);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "ValidateSegmentDuration failed: Invalid VideoStreamID %u: %" CHIP_ERROR_FORMAT, targetVideoStreamId,
                     err.Format());
        return false;
    }

    auto & allocatedVideoStreams = mCameraDevice->GetCameraHALInterface().GetAvailableVideoStreams();

    if (allocatedVideoStreams.empty())
    {
        ChipLogError(Camera, "ValidateSegmentDuration failed: No video streams available for validation");
        return false;
    }

    for (const VideoStream & stream : allocatedVideoStreams)
    {
        const VideoStreamStruct & videoStreamParams = stream.videoStreamParams;

        if (targetVideoStreamId == videoStreamParams.videoStreamID)
        {
            uint16_t keyFrameInterval = videoStreamParams.keyFrameInterval;

            if (keyFrameInterval == 0)
            {
                ChipLogError(Camera, "ValidateSegmentDuration failed: Key frame interval is 0 for video stream %u",
                             targetVideoStreamId);
                return false;
            }

            if (segmentDuration % keyFrameInterval != 0)
            {
                ChipLogError(Camera,
                             "ValidateSegmentDuration failed: Segment duration %ums is not a multiple of key frame interval %ums "
                             "for video stream %u",
                             segmentDuration, keyFrameInterval, targetVideoStreamId);
                return false;
            }

            if (segmentDuration < keyFrameInterval)
            {
                ChipLogError(
                    Camera,
                    "ValidateSegmentDuration failed: Segment duration %ums must be >= key frame interval %ums for video stream %u",
                    segmentDuration, keyFrameInterval, targetVideoStreamId);
                return false;
            }

            ChipLogProgress(Camera,
                            "ValidateSegmentDuration succeeded: %ums validated for video stream %u (key frame interval: %ums)",
                            segmentDuration, targetVideoStreamId, keyFrameInterval);
            return true;
        }
    }

    ChipLogError(Camera, "ValidateSegmentDuration failed: VideoStreamID %u not found in allocated video streams",
                 targetVideoStreamId);
    return false;
}

bool PushAvStreamTransportManager::ValidateUrl(const std::string & url)
{
    const std::string https = "https://";

    // Check minimum length and https prefix
    if (url.size() <= https.size() || url.substr(0, https.size()) != https)
    {
        return false;
    }

    // Check that URL does not contain fragment character '#'
    if (url.find('#') != std::string::npos)
    {
        ChipLogError(Camera, "URL contains fragment character '#'");
        return false;
    }

    // Check that URL does not contain query character '?'
    if (url.find('?') != std::string::npos)
    {
        ChipLogError(Camera, "URL contains query character '?'");
        return false;
    }

    // Check that URL ends with a forward slash '/'
    if (url.back() != '/')
    {
        ChipLogError(Camera, "URL does not end with '/'");
        return false;
    }

    // Check for non-empty host
    size_t hostStart = https.size();
    size_t hostEnd   = url.find('/', hostStart);
    std::string host = (hostEnd == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, hostEnd - hostStart);

    return !host.empty();
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::SelectVideoStream(StreamUsageEnum streamUsage,
                                                                                    uint16_t & videoStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return Status::Failure;
    }

    return GetVideoStreamIdForStreams(streamUsage, videoStreamId);
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::SelectAudioStream(StreamUsageEnum streamUsage,
                                                                                    uint16_t & audioStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return Status::Failure;
    }

    return GetAudioStreamIdForStreams(streamUsage, audioStreamId);
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::ValidateZoneId(uint16_t zoneId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return Status::Failure;
    }
    auto & zones = mCameraDevice->GetZoneManagementDelegate().GetZoneMgmtServer()->GetZones();

    for (const auto & zone : zones)
    {
        if (zone.zoneID == zoneId)
        {
            return Status::Success;
        }
    }
    return Status::Failure;
}

bool PushAvStreamTransportManager::ValidateMotionZoneListSize(size_t zoneListSize)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return false;
    }
    auto maxZones = mCameraDevice->GetZoneManagementDelegate().GetZoneMgmtServer()->GetMaxZones();
    if (zoneListSize >= maxZones)
    {
        return false;
    }
    return true;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::ValidateVideoStream(uint16_t videoStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return Status::Failure;
    }

    auto & avsmController = mCameraDevice->GetCameraAVStreamMgmtController();
    if (CHIP_NO_ERROR == avsmController.ValidateVideoStreamID(videoStreamId))
    {
        return Status::Success;
    }
    return Status::Failure;
}

Protocols::InteractionModel::Status PushAvStreamTransportManager::ValidateAudioStream(uint16_t audioStreamId)
{
    if (mCameraDevice == nullptr)
    {
        ChipLogError(Camera, "CameraDeviceInterface not initialized");
        return Status::Failure;
    }

    auto & avsmController = mCameraDevice->GetCameraAVStreamMgmtController();

    if (CHIP_NO_ERROR == avsmController.ValidateAudioStreamID(audioStreamId))
    {
        return Status::Success;
    }
    return Status::Failure;
}

PushAvStreamTransportStatusEnum PushAvStreamTransportManager::GetTransportBusyStatus(const uint16_t connectionID)
{
    if (mTransportMap.find(connectionID) == mTransportMap.end())
    {
        ChipLogError(Camera, "PushAvStreamTransportManager, failed to find Connection :[%u]", connectionID);
        return PushAvStreamTransportStatusEnum::kUnknown;
    }

    if (mTransportMap[connectionID].get()->GetBusyStatus())
    {
        return PushAvStreamTransportStatusEnum::kBusy;
    }
    else
    {
        return PushAvStreamTransportStatusEnum::kIdle;
    }
}

void PushAvStreamTransportManager::OnZoneTriggeredEvent(uint16_t zoneId)
{
    for (auto & pavst : mTransportMap)
    {
        int connectionId = pavst.first;
        ChipLogError(Camera, "PushAV sending trigger to connection ID %d", connectionId);

        if (mTransportOptionsMap[connectionId].triggerOptions.triggerType == TransportTriggerTypeEnum::kMotion)
        {
            pavst.second->TriggerTransport(TriggerActivationReasonEnum::kAutomation, zoneId, 10);
        }
    }
}

void PushAvStreamTransportManager::SetTLSCerts(Tls::CertificateTable::BufferedClientCert & clientCertEntry,
                                               Tls::CertificateTable::BufferedRootCert & rootCertEntry)
{
    auto rootSpan = rootCertEntry.GetCert().certificate.Value();
    mBufferRootCert.assign(rootSpan.data(), rootSpan.data() + rootSpan.size());

    auto clientSpan = clientCertEntry.GetCert().clientCertificate.Value().Value();
    mBufferClientCert.assign(clientSpan.data(), clientSpan.data() + clientSpan.size());

    mBufferIntermediateCerts.clear();
    if (clientCertEntry.mCertWithKey.detail.intermediateCertificates.HasValue())
    {
        auto intermediateList = clientCertEntry.mCertWithKey.detail.intermediateCertificates.Value();
        auto iter             = intermediateList.begin();
        while (iter.Next())
        {
            auto certSpan = iter.GetValue();
            std::vector<uint8_t> intermediateCert;
            intermediateCert.assign(certSpan.data(), certSpan.data() + certSpan.size());
            mBufferIntermediateCerts.push_back(intermediateCert);
        }
        if (iter.GetStatus() != CHIP_NO_ERROR)
        {
            ChipLogError(Camera, "Error iterating intermediate certificates: %" CHIP_ERROR_FORMAT, iter.GetStatus().Format());
            mBufferIntermediateCerts.clear();
        }
        else
        {
            ChipLogProgress(Camera, "Intermediate certificates fetched and stored. Size: %ld", mBufferIntermediateCerts.size());
        }
    }
    else
    {
        ChipLogProgress(Camera, "No intermediate certificates found.");
    }

    const ByteSpan rawKeySpan = clientCertEntry.mCertWithKey.key.Span();
    if (rawKeySpan.size() != Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length)
    {
        ChipLogError(Camera, "Raw key pair has incorrect size: %ld (expected %ld)", rawKeySpan.size(),
                     static_cast<size_t>(Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length));
        return;
    }

    Crypto::P256SerializedKeypair rawSerializedKeypair;
    if (rawSerializedKeypair.SetLength(rawKeySpan.size()) != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to set length for serialized keypair");
        return;
    }
    memcpy(rawSerializedKeypair.Bytes(), rawKeySpan.data(), rawKeySpan.size());

    uint8_t derBuffer[Credentials::kP256ECPrivateKeyDERLength];
    MutableByteSpan keypairDer(derBuffer);

    CHIP_ERROR err = Credentials::ConvertECDSAKeypairRawToDER(rawSerializedKeypair, keypairDer);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Camera, "Failed to convert raw keypair to DER: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    mBufferClientCertKey.assign(keypairDer.data(), keypairDer.data() + keypairDer.size());
}

void PushAvStreamTransportManager::OnAttributeChanged(AttributeId attributeId)
{
    ChipLogProgress(Zcl, "Attribute changed for AttributeId = " ChipLogFormatMEI, ChipLogValueMEI(attributeId));
}

CHIP_ERROR PushAvStreamTransportManager::LoadCurrentConnections(std::vector<TransportConfigurationStorage> & currentConnections)
{
    ChipLogProgress(Zcl, "Push AV Current Connections loaded");

    return CHIP_NO_ERROR;
}

CHIP_ERROR
PushAvStreamTransportManager::PersistentAttributesLoadedCallback()
{
    ChipLogProgress(Zcl, "Push AV Stream Transport Persistent attributes loaded");

    return CHIP_NO_ERROR;
}
