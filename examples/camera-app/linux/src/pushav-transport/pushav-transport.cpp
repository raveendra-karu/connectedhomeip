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

#include <pushav-transport.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

PushAVTransport::PushAVTransport(uint16_t connectionID, const char * url, TransportTriggerTypeEnum transportTriggerType)
{
    mConnectionID         = connectionID;
    mTransportTriggerType = transportTriggerType;
    mTransportStatus      = TransportStatusEnum::kInactive;
    serverUrl             = url;
}

void PushAVTransport::InitializeRecorder()
{
    if (!isRecorderInitialized)
    {
        recorder              = std::make_unique<PushAVClipRecorder>(clipInfo, audioInfo, videoInfo, uploader.get());
        isRecorderInitialized = true;
    }
    else
    {
        ChipLogError(Camera, "Recorder already initialized");
    }
    clipInfo.mClipId++;
}

PushAVTransport::~PushAVTransport()
{
    // TODO cleanup the existing recorded files here.
    mCanSendVideo = false;
    mCanSendAudio = false;
    recorder->Stop();
    isRecorderInitialized = false;
}

bool PushAVTransport::InBlindPeriod()
{
    if (blindStartTime == std::chrono::steady_clock::time_point())
    {
        return false;
    }
    else
    {
        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - blindStartTime).count();
        ChipLogProgress(Camera, "PushAVTransport blind period elapsed: %ld", elapsed);
        return (elapsed < recorder->mClipInfo.mBlindDuration);
    }
}

bool PushAVTransport::HandleTriggerDetected()
{
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - recorder->mClipInfo.activationTime).count();
    ChipLogError(Camera, "PushAVTransport HandleTriggerDetected elapsed: %ld", elapsed);
    if (InBlindPeriod())
    {
        return false;
    }

    if (!recorder->mRunning)
    {
        // Start new recording
        // recorder->mClipInfo.activationTime = std::chrono::steady_clock::now();
        hasAugmented = false;
        recorder->Start();
    }
    else
    {
        // Extend existing recording
        uint16_t previousDuration = recorder->mClipInfo.mInitialDuration - recorder->mClipInfo.mAugmentationDuration;

        if ((elapsed < recorder->mClipInfo.mInitialDuration) && (!hasAugmented || elapsed >= previousDuration))
        {
            ChipLogError(Camera, "PushAVTransport extending recording %d -> %d", recorder->mClipInfo.mInitialDuration,
                         static_cast<uint16_t>(std::min(static_cast<uint32_t>(recorder->mClipInfo.mInitialDuration +
                                                                              recorder->mClipInfo.mAugmentationDuration),
                                                        static_cast<uint32_t>(recorder->mClipInfo.mMaxClipDuration))));
            recorder->mClipInfo.mInitialDuration = static_cast<uint16_t>(
                std::min(static_cast<uint32_t>(recorder->mClipInfo.mInitialDuration + recorder->mClipInfo.mAugmentationDuration),
                         static_cast<uint32_t>(recorder->mClipInfo.mMaxClipDuration)));
            hasAugmented = true;
        }
        else
        {
            if (elapsed >= recorder->mClipInfo.mInitialDuration)
            {
                ChipLogError(Camera, "PushAVTransport starting blind period");
                blindStartTime = std::chrono::steady_clock::now();
                return false;
            }
        }
    }
    return true;
}

void PushAVTransport::TriggerTransport(TriggerActivationReasonEnum activationReason)
{
    ChipLogProgress(Camera, "PushAVTransport trigger transport, activation reason: [%u]", (uint16_t) activationReason);

    // TODO initialize recorder, check for running, do timecontrol
    if (mTransportTriggerType == TransportTriggerTypeEnum::kCommand || mTransportTriggerType == TransportTriggerTypeEnum::kMotion)
    {
        if (HandleTriggerDetected())
        {
            ChipLogError(Camera, "PushAVTransport command/motion transport trigger received. Clip duration [%d seconds]",
                         recorder->mClipInfo.mInitialDuration);
        }
        else
        {
            ChipLogError(Camera,
                         "PushAVTransport command/motion transport trigger received but ignored due to blind periodClip duration. "
                         "Clip duration [%d seconds]",
                         recorder->mClipInfo.mInitialDuration);
        }
    }

    if (mTransportTriggerType == TransportTriggerTypeEnum::kContinuous)
    {
        ChipLogProgress(Camera, "PushAVTransport continuous transport trigger received. No action needed");
        return;
    }
}

void PushAVTransport::setTransportStatus(TransportStatusEnum status)
{
    if (mTransportStatus == status)
    {
        return;
    }

    mTransportStatus = status;
    if (status == TransportStatusEnum::kActive)
    {
        mCanSendVideo = true;
        mCanSendAudio = true;
        uploader      = std::make_unique<PushAVUploader>();
        uploader->Start();
        InitializeRecorder();
        isUploaderInitialized = true;
        if (mTransportTriggerType == TransportTriggerTypeEnum::kContinuous)
        {
            recorder->Start();
        }
    }
    if (status == TransportStatusEnum::kInactive)
    {
        mCanSendVideo = false;
        mCanSendAudio = false;
        recorder->Stop();
        // TODO cleanup the existing recorded files here.
        // TODO deinitialize uploader
        isRecorderInitialized = false;
        isUploaderInitialized = false;
    }
}

// Implementation of SendVideo method
void PushAVTransport::SendVideo(const char * data, size_t size, uint16_t videoStreamID)
{
    if (!recorder)
    {
        ChipLogError(Camera, "Recorder is null in SendVideo");
        return;
    }
    if (recorder->mDeInitializeRecorder.load())
    {

        isRecorderInitialized = false;
        recorder.reset();
        InitializeRecorder();
        return;
    }

    if (CanSendVideo())
    {
        // ChipLogProgress(Camera, "MAGAGER:Sending Video Data");
        recorder->PushPacket(data, size, 1);
    }
}

// Implementation of SendAudio method
void PushAVTransport::SendAudio(const char * data, size_t size, uint16_t audioStreamID)
{
    if (!recorder)
    {
        ChipLogError(Camera, "Recorder is null in SendAudio");
        return;
    }
    if (recorder->mDeInitializeRecorder.load())
    {
        isRecorderInitialized = false;
        recorder.reset();
        InitializeRecorder();
        return;
    }

    if (CanSendAudio())
    {
        recorder->PushPacket(data, size, 0);
    }
}

void PushAVTransport::SendAudioVideo(const char * data, size_t size, uint16_t videoStreamID, uint16_t audioStreamID) {}

// Utility API for Test purpose
void PushAVTransport::readFromFile(char * filename, uint8_t ** videoBuffer, size_t * videoBufferBytes)
{
    const char * in_f_name = filename;
    FILE * infile;
    size_t result;
    /* open an existing file for reading */
    infile = fopen(in_f_name, "r");
    /* quit if the file does not exist */
    if (infile == nullptr)
    {
        return;
    }
    /* Get the number of bytes */
    fseek(infile, 0L, SEEK_END);
    *videoBufferBytes = ftell(infile);

    /* reset the file position indicator to the beginning of the file */
    fseek(infile, 0L, SEEK_SET);
    /* grab sufficient memory for the fileBuffer to hold the text */
    *videoBuffer = (uint8_t *) calloc(*videoBufferBytes, sizeof(uint8_t));
    /* memory error */
    if (*videoBuffer == nullptr)
    {
        fclose(infile);
        return;
    }

    /* copy all the text into the fileBuffer */
    result = fread(*videoBuffer, sizeof(uint8_t), *videoBufferBytes, infile);
    fclose(infile);
    if ((size_t) result != *videoBufferBytes)
    {
        return;
    }
}

// Implementation of CanSendVideo method
bool PushAVTransport::CanSendVideo()
{
    return mCanSendVideo;
}

// Dummy implementation of CanSendAudio method
bool PushAVTransport::CanSendAudio()
{
    return mCanSendAudio;
}
