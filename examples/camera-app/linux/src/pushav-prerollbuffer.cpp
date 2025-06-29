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

#include "pushav-prerollbuffer.h"

PushAvPreRollBuffer::PushAvPreRollBuffer(long long maxDurationMs)
{
    if (maxDurationMs <= 0)
    {
        ChipLogError(Camera, "PushAV - PrerollBuffer initialized with invalid max duration : %lld ms", maxDurationMs);
        maxDurationMs = 1;
    }
    if (maxDurationMs > 5)
    {
        maxDurationMs = 5;
    }
    mMaxDurationMs = maxDurationMs;
    ChipLogProgress(Camera, "PushAV - PrerollBuffer initialized with max duration : %lld ms", mMaxDurationMs);
}

PushAvPreRollBuffer::~PushAvPreRollBuffer()
{
    ChipLogProgress(Camera, "PushAV - PrerollBuffer destroyed");
    std::lock_guard<std::mutex> lock(mPreRollBufferMutex);
    for (PreRollInputPacket & input : mBuffer)
    {
        av_packet_free(&input.packet);
    }
    mBuffer.clear();
}

void PushAvPreRollBuffer::AddPacket(AVPacket * packet, bool isVideo)
{
    if (!packet)
    {
        ChipLogError(Camera, "ERROR: Invalid AV Packet!");
    }
    std::lock_guard<std::mutex> lock(mPreRollBufferMutex);
    AVPacket * packet_clone = av_packet_clone(packet);
    if (!packet_clone)
    {
        ChipLogError(Camera, "ERROR: Failed to clone AV Packet!");
        return;
    }
    mBuffer.push_back(PreRollInputPacket(packet, std::chrono::steady_clock::now(), isVideo));
    RemovePackets(true); // remove as per mMaxDurationMs
}

std::pair<AVPacket *, bool> PushAvPreRollBuffer::FetchPacket()
{
    std::lock_guard<std::mutex> lock(mPreRollBufferMutex);
    if (GetSize() == 0)
    {
        return std::make_pair(nullptr, false);
    }
    PreRollInputPacket & front = mBuffer.front();
    AVPacket * packet          = av_packet_clone(front.packet);
    bool isVideo               = front.isVideo;
    av_packet_free(&front.packet);
    mBuffer.pop_front();
    return std::make_pair(packet, isVideo);
}

int PushAvPreRollBuffer::GetSize()
{
    std::lock_guard<std::mutex> lock(mPreRollBufferMutex);
    return mBuffer.size();
}

void PushAvPreRollBuffer::RemovePackets(bool followMaxDuration)
{
    if (mMaxDurationMs <= 0)
        return;
    auto now = std::chrono::steady_clock::now();
    while (!mBuffer.empty())
    {
        auto & front = mBuffer.front();
        auto ageMs   = std::chrono::duration_cast<std::chrono::milliseconds>(now - front.InputTime).count();
        if (!followMaxDuration || ageMs > mMaxDurationMs)
        {
            av_packet_free(&front.packet);
            mBuffer.pop_front();
        }
        else
        {
            break; // all packets are younger than max duration, stop here.
        }
    }
}
