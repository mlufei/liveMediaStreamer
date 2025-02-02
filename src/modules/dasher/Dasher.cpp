/*
 *  Dasher.cpp - Class that handles DASH sessions
 *  Copyright (C) 2014  Fundació i2CAT, Internet i Innovació digital a Catalunya
 *
 *  This file is part of liveMediaStreamer.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Authors:  Marc Palau <marc.palau@i2cat.net>
 *            Gerard Castillo <gerard.castillo@i2cat.net>
 */

#include "Dasher.hh"
#include "../../AVFramedQueue.hh"
#include "DashVideoSegmenter.hh"
#include "DashVideoSegmenterAVC.hh"
#include "DashVideoSegmenterHEVC.hh"
#include "DashAudioSegmenter.hh"

#include <map>
#include <string>
#include <chrono>
#include <fstream>
#include <unistd.h>
#include <math.h>

Dasher::Dasher(unsigned readersNum) :
TailFilter(readersNum), mpdMngr(NULL), hasVideo(false), videoStarted(false), timestampOffset(std::chrono::microseconds(0))
{
    fType = DASHER;
    initializeEventMap();
}

Dasher::~Dasher()
{
    for (auto seg : segmenters) {
        delete seg.second;
    }
    delete mpdMngr;
}

bool Dasher::configure(std::string dashFolder, std::string baseName_, size_t segDurInSec, size_t maxSeg, size_t minBuffTime)
{
    if (access(dashFolder.c_str(), W_OK) != 0) {
        utils::errorMsg("Error configuring Dasher: provided folder is not writable");
        return false;
    }

    if (dashFolder.back() != '/') {
        dashFolder.append("/");
    }

    if (baseName_.empty() || segDurInSec == 0) {
        utils::errorMsg("Error configuring Dasher: provided parameters are not valid");
        return false;
    }

    basePath = dashFolder;
    baseName = baseName_;
    mpdPath = basePath + baseName + ".mpd";
    vSegTempl = baseName + "_$RepresentationID$_$Time$.m4v";
    aSegTempl = baseName + "_$RepresentationID$_$Time$.m4a";
    vInitSegTempl = baseName + "_$RepresentationID$_init.m4v";
    aInitSegTempl = baseName + "_$RepresentationID$_init.m4a";

    if (!mpdMngr){
        mpdMngr = new MpdManager();
    }
    
    mpdMngr->configure(minBuffTime, maxSeg, segDurInSec);
    segDur = std::chrono::seconds(segDurInSec);

    return true;
}

bool Dasher::doProcessFrame(std::map<int, Frame*> &orgFrames, std::vector<int> newFrames)
{
    DashSegmenter* segmenter;
    Frame* frame;

    if (!mpdMngr) {
        utils::errorMsg("Dasher MUST be configured in order to process frames");
        return false;
    }

    for (auto id : newFrames) {
        segmenter = getSegmenter(id);

        if (!segmenter) {
            continue;
        }
        
        if (timestampOffset.count() == 0){
            timestampOffset = orgFrames[id]->getPresentationTime();
            for(auto seg : segmenters){
                seg.second->setOffset(timestampOffset);
            }
        }

        frame = segmenter->manageFrame(orgFrames[id]);

        if (!frame) {
            continue;
        }

        if (!generateInitSegment(id, segmenter)) {
            utils::errorMsg("[Dasher::doProcessFrame] Error generating init segment");
            continue;
        }

        if (generateSegment(id, frame, segmenter)) {
            utils::debugMsg("[Dasher::doProcessFrame] New segment generated");
        }

        if (!appendFrameToSegment(id, frame, segmenter)) {
            utils::errorMsg("[Dasher::doProcessFrame] Error appnding frame to segment");
            continue;
        }
    }

    if (writeVideoSegments()) {
        utils::debugMsg("[Dasher::doProcessFrame] Video segments to disk");
    }

    if (writeAudioSegments()) {
        utils::debugMsg("[Dasher::doProcessFrame] Audio segments to disk");
    }

    return true;
}

bool Dasher::appendFrameToSegment(size_t id, Frame* frame, DashSegmenter* segmenter)
{
    DashVideoSegmenter* vSeg;
    DashAudioSegmenter* aSeg;

    if ((vSeg = dynamic_cast<DashVideoSegmenter*>(segmenter)) != NULL) {

        if (!vSeg->appendFrameToDashSegment(frame)) {
            utils::errorMsg("Error appending video frame to segment");
            return false;
        }

    }   

    if ((aSeg = dynamic_cast<DashAudioSegmenter*>(segmenter)) != NULL) {

        if (!aSeg->appendFrameToDashSegment(frame)) {
            utils::errorMsg("Error appending audio frame to segment");
            return false;
        }
    }

    if (!vSeg && !aSeg) {
        return false;
    }

    return true;
}

bool Dasher::generateInitSegment(size_t id, DashSegmenter* segmenter)
{
    DashVideoSegmenter* vSeg;
    DashAudioSegmenter* aSeg;

    if ((vSeg = dynamic_cast<DashVideoSegmenter*>(segmenter)) != NULL) {

        if (!vSeg->generateInitSegment(initSegments[id])) {
            return true;
        }

        if(!initSegments[id]->writeToDisk(getInitSegmentName(basePath, baseName, id, V_EXT))) {
            utils::errorMsg("Error writing DASH segment to disk: invalid path");
            return false;
        }
    }

    if ((aSeg = dynamic_cast<DashAudioSegmenter*>(segmenter)) != NULL) {

        if (!aSeg->generateInitSegment(initSegments[id])) {
            return true;
        }

        if(!initSegments[id]->writeToDisk(getInitSegmentName(basePath, baseName, id, A_EXT))) {
            utils::errorMsg("Error writing DASH segment to disk: invalid path");
            return false;
        }
    }

    if (!vSeg && !aSeg) {
        return false;
    }

    return true;
}

bool Dasher::generateSegment(size_t id, Frame* frame, DashSegmenter* segmenter)
{
    DashVideoSegmenter* vSeg;
    DashAudioSegmenter* aSeg;

    if ((vSeg = dynamic_cast<DashVideoSegmenter*>(segmenter)) != NULL) {

        if (!vSeg->generateSegment(vSegments[id], frame)) {
            return false;
        }

        mpdMngr->updateVideoAdaptationSet(V_ADAPT_SET_ID, segmenters[id]->getTimeBase(), vSegTempl, vInitSegTempl);
        mpdMngr->updateVideoRepresentation(V_ADAPT_SET_ID, std::to_string(id), vSeg->getVideoFormat(), vSeg->getWidth(),
                                            vSeg->getHeight(), vSeg->getBitrate(), vSeg->getFramerate());

    }

    if (!hasVideo && (aSeg = dynamic_cast<DashAudioSegmenter*>(segmenter)) != NULL) {
        
        if (!aSeg->generateSegment(aSegments[id], frame)) {
            return false;
        }

        mpdMngr->updateAudioAdaptationSet(A_ADAPT_SET_ID, segmenters[id]->getTimeBase(), aSegTempl, aInitSegTempl);
        mpdMngr->updateAudioRepresentation(A_ADAPT_SET_ID, std::to_string(id), AUDIO_CODEC, 
                                            aSeg->getSampleRate(), aSeg->getBitrate(), aSeg->getChannels());
    }

    if (!vSeg && !aSeg) {
        return false;
    }

    return true;
}

bool Dasher::writeVideoSegments()
{
    size_t ts;
    size_t dur;
    size_t rmTimestamp;

    if (vSegments.empty()) {
        return false;
    }

    for (auto seg : vSegments) {
        if (!seg.second->isComplete()) {
            return false;
        }
    }

    if (!forceAudioSegmentsGeneration()) {
        utils::warningMsg("Forcing the generation of audio segments. This may cause errors!");
    }

    ts = vSegments.begin()->second->getTimestamp();
    dur = vSegments.begin()->second->getDuration();

    for (auto seg : vSegments) {
        if (seg.second->getTimestamp() != ts || seg.second->getDuration() != dur) {
            utils::warningMsg("Segments of the same adaptation set have different timestamps");
        }
    }

    if (!writeSegmentsToDisk(vSegments, ts, V_EXT)) {
        utils::errorMsg("Error writing DASH video segment to disk");
        return false;
    }

    rmTimestamp = mpdMngr->updateAdaptationSetTimestamp(V_ADAPT_SET_ID, ts, dur);

    mpdMngr->writeToDisk(mpdPath.c_str());

    if (rmTimestamp > 0 && !cleanSegments(vSegments, rmTimestamp, V_EXT)) {
        utils::warningMsg("Error cleaning dash video segments");
    }

    return true;
}

bool Dasher::writeAudioSegments()
{
    size_t ts;
    size_t dur;
    size_t rmTimestamp;

    if (aSegments.empty()) {
        return false;
    }

    for (auto seg : aSegments) {
        if (!seg.second->isComplete()) {
            return false;
        }
    }

    ts = aSegments.begin()->second->getTimestamp();
    dur = aSegments.begin()->second->getDuration();

    for (auto seg : aSegments) {
        if (seg.second->getTimestamp() != ts || seg.second->getDuration() != dur) {
            utils::warningMsg("Segments of the same adaptation set have different timestamps");
        }
    }

    if (!writeSegmentsToDisk(aSegments, ts, A_EXT)) {
        utils::errorMsg("Error writing DASH video segment to disk");
        return false;
    }

    rmTimestamp = mpdMngr->updateAdaptationSetTimestamp(A_ADAPT_SET_ID, ts, dur);

    mpdMngr->writeToDisk(mpdPath.c_str());

    if (rmTimestamp > 0 && !cleanSegments(aSegments, rmTimestamp, A_EXT)) {
        utils::warningMsg("Error cleaning dash video segments");
    }

    return true;
}

bool Dasher::forceAudioSegmentsGeneration()
{
    DashSegmenter* segmenter;
    DashAudioSegmenter* aSeg;

    for (auto seg : aSegments) {
        segmenter = getSegmenter(seg.first);

        if (!segmenter) {
            continue;
        }

        aSeg = dynamic_cast<DashAudioSegmenter*>(segmenter);

        if (!aSeg) {
            continue;
        }

        if (!aSeg->generateSegment(seg.second, NULL, true)) {
            utils::errorMsg("Error forcing audio segment generation");
            return false;
        }

        mpdMngr->updateAudioAdaptationSet(A_ADAPT_SET_ID, segmenters[seg.first]->getTimeBase(), aSegTempl, aInitSegTempl);
        mpdMngr->updateAudioRepresentation(A_ADAPT_SET_ID, std::to_string(seg.first), AUDIO_CODEC, 
                                            aSeg->getSampleRate(), aSeg->getBitrate(), aSeg->getChannels());
    }

    return true;
}

bool Dasher::writeSegmentsToDisk(std::map<int,DashSegment*> segments, size_t timestamp, std::string segExt)
{
    for (auto seg : segments) {

        if(!seg.second->writeToDisk(getSegmentName(basePath, baseName, seg.first, timestamp, segExt))) {
            utils::errorMsg("Error writing DASH segment to disk: invalid path");
            return false;
        }

        seg.second->clear();
        seg.second->incrSeqNumber();
    }

    return true;
}

bool Dasher::cleanSegments(std::map<int,DashSegment*> segments, size_t timestamp, std::string segExt)
{
    bool success = true;
    std::string segmentName;

    for (auto seg : segments) {
        segmentName = getSegmentName(basePath, baseName, seg.first, timestamp, segExt);

        if (std::remove(segmentName.c_str()) != 0) {
            success &= false;
            utils::warningMsg("Error cleaning dash segment: " + segmentName);
        }
    }

    return success;
}


void Dasher::initializeEventMap()
{
    eventMap["configure"] = std::bind(&Dasher::configureEvent, this, std::placeholders::_1);
    eventMap["setBitrate"] = std::bind(&Dasher::setBitrateEvent, this, std::placeholders::_1);
}

void Dasher::doGetState(Jzon::Object &filterNode)
{
    Jzon::Array readersList;

    filterNode.Add("folder", basePath);
    filterNode.Add("baseName", baseName);
    filterNode.Add("mpdURI", mpdPath);
    filterNode.Add("segDurInSec", std::to_string(segDur.count()));
    filterNode.Add("maxSegments", std::to_string(mpdMngr->getMaxSeg()));
    filterNode.Add("minBufferTime", std::to_string(mpdMngr->getMinBuffTime()));
    
    for (auto it : segmenters) {
        readersList.Add(it.first);
    }

    filterNode.Add("readers", readersList);
}

bool Dasher::configureEvent(Jzon::Node* params)
{
    std::string dashFolder = basePath;
    std::string bName = baseName;
    size_t segDurInSec = segDur.count();
    size_t maxSeg = 0;
    size_t minBuffTime = 0;

    if (!params) {
        return false;
    }

    if (params->Has("folder") && params->Get("folder").IsString()) {
        dashFolder = params->Get("folder").ToString();
    }

    if (params->Has("baseName") && params->Get("baseName").IsString()) {
        bName = params->Get("baseName").ToString();
    }

    if (params->Has("segDurInSec") && params->Get("segDurInSec").IsNumber()) {
        segDurInSec = params->Get("segDurInSec").ToInt();
    }
    
    if (params->Has("maxSeg") && params->Get("maxSeg").IsNumber()) {
        maxSeg = params->Get("maxSeg").ToInt();
    } else if (mpdMngr){
        maxSeg = mpdMngr->getMaxSeg();
    }
    
    if (params->Has("minBuffTime") && params->Get("minBuffTime").IsNumber()) {
        minBuffTime = params->Get("minBuffTime").ToInt();
    } else if (mpdMngr){
        minBuffTime = mpdMngr->getMinBuffTime();
    }

    return configure(dashFolder, bName, segDurInSec, maxSeg, minBuffTime);
}

bool Dasher::setBitrateEvent(Jzon::Node* params)
{
    int id, bitrate;

    if (!params) {
        return false;
    }

    if (!params->Has("id") || !params->Has("bitrate")) {
        return false;
    }

    id = params->Get("id").ToInt();
    bitrate = params->Get("bitrate").ToInt();

    return setDashSegmenterBitrate(id, bitrate);
}

bool Dasher::specificReaderConfig(int readerId, FrameQueue* queue)
{
    VideoFrameQueue *vQueue;
    AudioFrameQueue *aQueue;
    std::string completeSegBasePath;

    if (!mpdMngr) {
        utils::errorMsg("Dasher MUST be configured in order to add a mew segmenter");
        return false;
    }

    if (segmenters.count(readerId) > 0) {
        utils::errorMsg("Error adding segmenter: there is a segmenter already assigned to this reader");
        return false;
    }

    if ((vQueue = dynamic_cast<VideoFrameQueue*>(queue)) != NULL) {

        if (vQueue->getStreamInfo()->video.codec != H264 && vQueue->getStreamInfo()->video.codec != H265) {
            utils::errorMsg("Error setting dasher reader: only H264 & H265 codecs are supported for video");
            return false;
        }

        if (vQueue->getStreamInfo()->video.codec == H264) {
            segmenters[readerId] = new DashVideoSegmenterAVC(segDur, timestampOffset);
        } else if (vQueue->getStreamInfo()->video.codec == H265) {
            segmenters[readerId] = new DashVideoSegmenterHEVC(segDur, timestampOffset);
        } else {
            utils::errorMsg("Error setting dasher video segmenter: only H264 & H265 codecs are supported for video");
            return false;
        }
        vSegments[readerId] = new DashSegment();
        initSegments[readerId] = new DashSegment();
        hasVideo = true;
    }

    if ((aQueue = dynamic_cast<AudioFrameQueue*>(queue)) != NULL) {

        if (aQueue->getStreamInfo()->audio.codec != AAC) {
            utils::errorMsg("Error setting Dasher reader: only AAC codec is supported for audio");
            return false;
        }

        segmenters[readerId] = new DashAudioSegmenter(segDur, timestampOffset);
        aSegments[readerId] = new DashSegment();
        initSegments[readerId] = new DashSegment();
    }

    return true;
}

bool Dasher::specificReaderDelete(int readerId)
{
    if (segmenters.count(readerId) <= 0) {
        utils::errorMsg("Error removing DASH segmenter: no segmenter associated to provided reader");
        return false;
    }

    if (vSegments.count(readerId) > 0) {
        delete vSegments[readerId];
        vSegments.erase(readerId);
        mpdMngr->removeRepresentation(V_ADAPT_SET_ID, std::to_string(readerId));
    }

    if (aSegments.count(readerId) > 0) {
        delete aSegments[readerId];
        aSegments.erase(readerId);
        mpdMngr->removeRepresentation(A_ADAPT_SET_ID, std::to_string(readerId));
    }

    if (initSegments.count(readerId) > 0) {
        delete initSegments[readerId];
        initSegments.erase(readerId);
    }

    delete segmenters[readerId];
    segmenters.erase(readerId);

    if (vSegments.empty()) {
        hasVideo = false;
        videoStarted = false;
    }

    mpdMngr->writeToDisk(mpdPath.c_str());
    return true;
}

std::string Dasher::getSegmentName(std::string basePath, std::string baseName, size_t reprId, size_t timestamp, std::string ext)
{
    std::string fullName;
    fullName = basePath + baseName + "_" + std::to_string(reprId) + "_" + std::to_string(timestamp) + ext;

    return fullName;
}

std::string Dasher::getInitSegmentName(std::string basePath, std::string baseName, size_t reprId, std::string ext)
{
    std::string fullName;
    fullName = basePath + baseName + "_" + std::to_string(reprId) + "_init" + ext;

    return fullName;
}

DashSegmenter* Dasher::getSegmenter(size_t id)
{
    if (segmenters.count(id) <= 0) {
        return NULL;
    }

    return segmenters[id];
}

bool Dasher::setDashSegmenterBitrate(int id, size_t bps)
{
    DashSegmenter* segmenter;

    segmenter = getSegmenter(id);

    if (!segmenter) {
        utils::errorMsg("Error setting bitrate. Provided id does not exist");
        return false;
    }

    segmenter->setBitrate(bps);
    return true;
}

///////////////////
// DashSegmenter //
///////////////////

DashSegmenter::DashSegmenter(std::chrono::seconds segmentDuration, size_t tBase, std::chrono::microseconds offset) :
segDur(segmentDuration), dashContext(NULL), timeBase(tBase), frameDuration(0), currentTimestamp(0),
sequenceNumber(0), bitrateInBitsPerSec(0), tsOffset(offset)
{
    segDurInTimeBaseUnits = segDur.count()*timeBase;
}

DashSegmenter::~DashSegmenter()
{
    if(dashContext){
        free(dashContext);
    }
}

bool DashSegmenter::generateSegment(DashSegment* segment, Frame* frame, bool force)
{
    size_t segmentSize = 0;
    uint64_t segTimestamp;
    uint32_t segDuration;
    std::chrono::microseconds frameTs = std::chrono::microseconds(0);

    if (!frame && !force) {
        return false;
    }

    if (frame) {
        frameTs = frame->getPresentationTime();
    }

    segmentSize = customGenerateSegment(segment->getDataBuffer(), frameTs, segTimestamp, segDuration, force);

    if (segmentSize <= I2ERROR_MAX) {
        return false;
    }
    
    segment->setTimestamp(segTimestamp);
    segment->setDuration(segDuration);
    segment->setDataLength(segmentSize);
    segment->setComplete(true);
    segment->setSeqNumber(++sequenceNumber);
    
    if (currentTimestamp != segDuration){
        currentTimestamp = segTimestamp;
    }
    
    currentTimestamp += segDuration;
    return true;
}

size_t DashSegmenter::microsToTimeBase(std::chrono::microseconds microValue)
{
    return (microValue-tsOffset).count()*timeBase/std::micro::den;
}


/////////////////
// DashSegment //
/////////////////

DashSegment::DashSegment(size_t maxSize) : 
dataLength(0), seqNumber(0), timestamp(0), duration(0), complete(false)
{
    data = new unsigned char[maxSize];
}

DashSegment::~DashSegment()
{
    delete[] data;
}

void DashSegment::setSeqNumber(size_t seqNum)
{
    seqNumber = seqNum;
}

void DashSegment::setDataLength(size_t length)
{
    dataLength = length;
}

bool DashSegment::writeToDisk(std::string path)
{
    const char* p = path.c_str();
    std::ofstream file(p, std::ofstream::binary);

    if (!file) {
        return false;
    }

    file.write((char*)data, dataLength);
    file.close();
    return true;
}

void DashSegment::setTimestamp(size_t ts)
{
    timestamp = ts;
}

void DashSegment::setDuration(size_t dur)
{
    duration = dur;
}

void DashSegment::clear()
{
    timestamp = 0;
    duration = 0;
    dataLength = 0;
    complete = false;
}
