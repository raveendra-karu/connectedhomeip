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

#define IS_H264_FRAME_NALU_HEAD(frame)                                                                                             \
    (((frame)[0] == 0x00) && ((frame)[1] == 0x00) && (((frame)[2] == 0x01) || (((frame)[2] == 0x00) && ((frame)[3] == 0x01))))

PushAVTransport::PushAVTransport(uint16_t connectionID, const char * url, TransportTriggerTypeEnum transportTriggerType)
{
    mConnectionID         = connectionID;
    mTransportTriggerType = transportTriggerType;
    mTransportStatus      = TransportStatusEnum::kInactive;
    serverUrl             = url;
}

void PushAVTransport::InitializeRecorder()
{
    if (recorder)
    {
        recorder = std::make_unique<PushAVClipRecorder>(clipInfo, audioInfo, videoInfo, uploader.get());
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
    recorder.reset();
    uploader.reset();
    if (prerollBuffer)
    {
        prerollBuffer->~PushAvPreRollBuffer();
        delete prerollBuffer;
    }
}
bool IsH264IFrame(const uint8_t * data, unsigned int length)
{
    unsigned int idx = 0;
    int frameType    = 0;
    int foundSps     = 0;
    int foundPps     = 0;
    int foundIdr     = 0;
    bool ret         = false;

    if (data == nullptr || (length < 5))
    {
        return ret;
    }

    do
    {
        if (IS_H264_FRAME_NALU_HEAD(data + idx))
        {
            if (data[idx + 2] == 0x01)
                frameType = data[idx + 3] & 0x1f;
            else if ((data[idx + 2] == 0x00) && (data[idx + 3] == 0x01))
                frameType = data[idx + 4] & 0x1f;

            if (frameType == 7)
            {
                foundSps = 1;
            }
            else if (frameType == 8)
            {
                foundPps = 1;
            }
            else if (frameType == 5)
            {
                foundIdr = 1;
                break;
            }
            if ((data[idx + 2] == 0x00) && (data[idx + 3] == 0x01))
                idx++;

            idx += 4;
        }
        else
        {
            idx++;
        }
    } while (idx < (length - 4));

    if (foundSps == 1 && foundPps == 1 && foundIdr == 1)
    {
        ret = true;
    }

    return ret;
}

AVPacket * CreatePacket(const uint8_t * data, int size, bool isVideo)
{
    AVPacket * packet = av_packet_alloc();
    if (!packet)
    {
        ChipLogError(Camera, "ERROR: AVPacket memory allocation failed!");
        return nullptr;
    }
    packet->data = (uint8_t *) av_malloc(size);
    if (!packet->data)
    {
        ChipLogError(Camera, "ERROR: AVPacket data allocation failed!");
        av_packet_free(&packet);
        return nullptr;
    }
    memcpy(packet->data, data, size);
    packet->size = size;
    if (isVideo && IsH264IFrame(data, size))
    {
        packet->flags = AV_PKT_FLAG_KEY;
    }

    return packet;
}

bool InBlindPeriod(std::chrono::steady_clock::time_point blindStartTime, uint16_t blindDuration)
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
        return ((elapsed >= 0) && (elapsed < blindDuration));
    }
}

bool PushAVTransport::HandleTriggerDetected()
{
    int64_t elapsed;
    auto now = std::chrono::steady_clock::now();

    if (InBlindPeriod(blindStartTime, recorder->mClipInfo.mBlindDuration))
    {
        return false;
    }

    elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - recorder->mClipInfo.activationTime).count();
    ChipLogError(Camera, "PushAVTransport HandleTriggerDetected elapsed: %ld", elapsed);

    if (!recorder->mRunning)
    {
        // Start new recording
        hasAugmented                       = false;
        recorder->mClipInfo.activationTime = std::chrono::steady_clock::now();
        recorder->Start();
        mStreaming = true;
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
            mStreaming   = true;
        }
    }
    blindStartTime = recorder->mClipInfo.activationTime + std::chrono::seconds(recorder->mClipInfo.mInitialDuration);
    return true;
}

void PushAVTransport::TriggerTransport(TriggerActivationReasonEnum activationReason)
{
    ChipLogProgress(Camera, "PushAVTransport trigger transport, activation reason: [%u]", (uint16_t) activationReason);

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
        ChipLogProgress(Camera, "PushAVTransport transport status unchanged");
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
        if (mTransportTriggerType == TransportTriggerTypeEnum::kContinuous)
        {
            recorder->Start();
            return;
        }
        if (!prerollBuffer && clipInfo.mPreRollLength > 0)
        {
            prerollBuffer = new PushAvPreRollBuffer(clipInfo.mPreRollLength);
        }
        if (status == TransportStatusEnum::kInactive)
        {
            mCanSendVideo = false;
            mCanSendAudio = false;
            recorder.reset();
            uploader.reset();
            if (prerollBuffer)
            {
                prerollBuffer->~PushAvPreRollBuffer();
                delete prerollBuffer;
            }
        }
    }
}

bool PushAVTransport::IsStreaming()
{
    return mStreaming && (mTransportStatus == TransportStatusEnum::kActive);
}

void PushAVTransport::SendPacketsToRecorder()
{
    if (!IsStreaming())
    {
        return;
    }
    std::pair<AVPacket *, bool> packet = prerollBuffer->FetchPacket();
    while (packet.first != nullptr)
    {
        if (recorder->mDeInitializeRecorder.load())
        {
            recorder.reset();
            InitializeRecorder();
            mStreaming = false;
            return;
        }
        recorder->PushPacket(packet.first, packet.second);
    }
}

// Implementation of SendVideo method
void PushAVTransport::SendVideo(const char * data, size_t size, uint16_t videoStreamID)
{
    if (!CanSendVideo())
    {
        return;
    }
    AVPacket * packet = CreatePacket((const uint8_t *) data, size, true);
    if (prerollBuffer)
    {
        prerollBuffer->AddPacket(packet, 1);
    }
    SendPacketsToRecorder();
}

// Implementation of SendAudio method
void PushAVTransport::SendAudio(const char * data, size_t size, uint16_t audioStreamID)
{
    if (!CanSendAudio())
    {
        return;
    }
    AVPacket * packet = CreatePacket((const uint8_t *) data, size, false);
    if (prerollBuffer)
    {
        prerollBuffer->AddPacket(packet, 0);
    }
    SendPacketsToRecorder();
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
