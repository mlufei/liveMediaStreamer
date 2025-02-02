/*
 *  AudioEncoderLibav - A libav-based audio encoder
 *  Copyright (C) 2014  Fundació i2CAT, Internet i Innovació digital a Catalunya
 *
 *  This file is part of media-streamer.
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
 */

#include "AudioEncoderLibav.hh"
#include "../../AVFramedQueue.hh"
#include "../../AudioCircularBuffer.hh"
#include "../../Utils.hh"

bool checkSampleFormat(AVCodec *codec, enum AVSampleFormat sampleFmt);
bool checkSampleRateSupport(AVCodec *codec, int sampleRate);
bool checkChannelLayoutSupport(AVCodec *codec, uint64_t channelLayout);

AudioEncoderLibav::AudioEncoderLibav() : OneToOneFilter(),
        samplesPerFrame(0), internalLibavSampleFmt(AV_SAMPLE_FMT_NONE),
        outputBitrate(0), inputChannels(0), inputSampleRate(0), inputSampleFmt(S_NONE),
        inputLibavSampleFmt(AV_SAMPLE_FMT_NONE)
{
    avcodec_register_all();

    codec = NULL;
    codecCtx = NULL;
    resampleCtx = NULL;
    libavFrame = av_frame_alloc();
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    fType = AUDIO_ENCODER;

    outputStreamInfo = new StreamInfo (AUDIO);
    outputStreamInfo->audio.codec = AC_NONE;
    outputStreamInfo->audio.sampleRate = 0;
    outputStreamInfo->audio.channels = 0;
    outputStreamInfo->audio.sampleFormat = S_NONE;

    framerateMod = 1;

    currentTime = std::chrono::microseconds(0);

    initializeEventMap();
}

AudioEncoderLibav::~AudioEncoderLibav()
{
    avcodec_close(codecCtx);
    av_free(codecCtx);
    swr_free(&resampleCtx);
    av_free(libavFrame);
    av_packet_unref(&pkt);
}

FrameQueue* AudioEncoderLibav::allocQueue(ConnectionData cData)
{
    return AudioFrameQueue::createNew(cData, outputStreamInfo, DEFAULT_AUDIO_FRAMES);
}

bool AudioEncoderLibav::doProcessFrame(Frame *org, Frame *dst)
{     
    int ret, gotFrame, samples;
    AudioFrame* rawFrame;
    AudioFrame* codedFrame;

    rawFrame = dynamic_cast<AudioFrame*>(org);
    codedFrame = dynamic_cast<AudioFrame*>(dst);

    if (!rawFrame || !codedFrame) {
        utils::errorMsg("Error encoding audio frame: org or dst frames are not valid");
        return false;
    }

    if(!reconfigure(rawFrame)) {
        utils::errorMsg("Error reconfiguring audio encoder");
        return false;
    }

    //set up buffer and buffer length pointers
    pkt.data = codedFrame->getDataBuf();
    pkt.size = codedFrame->getMaxLength();

    //resample in order to adapt to encoder constraints
    samples = resample(rawFrame, libavFrame);

    if (samples <= 0) {
        utils::errorMsg("Error encoding audio frame: resampling error");
        return false;
    }

    ret = avcodec_encode_audio2(codecCtx, &pkt, libavFrame, &gotFrame);

    if (ret < 0) {
        utils::errorMsg("Error encoding audio frame");
        return false;
    }

    if (!gotFrame) {
        utils::warningMsg("Could not encode audio frame");
        return false;
    }

    codedFrame->setLength(pkt.size);
    codedFrame->setSamples(samples);

    dst->setConsumed(true);
    dst->setPresentationTime(org->getPresentationTime());
    dst->setOriginTime(org->getOriginTime());
    dst->setSequenceNumber(org->getSequenceNumber());
    
    return true;
}


bool AudioEncoderLibav::specificReaderConfig(int /*readerID*/, FrameQueue* queue)
{
    AudioCircularBuffer* b;

    if (samplesPerFrame == 0) {
        utils::errorMsg("Error setting audio encoder reader. Samples per frame has 0 value");
        return false;
    }

    b = dynamic_cast<AudioCircularBuffer*>(queue);
    
    if (!b) {
        utils::errorMsg("[AudioEncoderLibav::setReader] Input queue must be an AudioCircularBuffer");
        return false;
    }

    b->setOutputFrameSamples(samplesPerFrame);

    return true;
}

bool AudioEncoderLibav::configure0(ACodecType codec, int codedAudioChannels, int codedAudioSampleRate, int bitrate)
{
    AVCodecID codecId;

    if (getCodec() != AC_NONE) {
        utils::errorMsg("Audio encoder is already configured");
        return false;
    }

    outputStreamInfo->audio.codec = codec;
    outputStreamInfo->setCodecDefaults();
    outputStreamInfo->audio.channels = codedAudioChannels;
    outputStreamInfo->audio.sampleRate = codedAudioSampleRate;
    outputBitrate = bitrate;

    switch(getCodec()) {
        case PCM:
            codecId = AV_CODEC_ID_PCM_S16BE;
            internalLibavSampleFmt = AV_SAMPLE_FMT_S16;
            outputStreamInfo->audio.sampleFormat = S16;
            break;
        case PCMU:
            codecId = AV_CODEC_ID_PCM_MULAW;
            internalLibavSampleFmt = AV_SAMPLE_FMT_S16;
            outputStreamInfo->audio.sampleFormat = S16;
            break;
        case OPUS:
            codecId = AV_CODEC_ID_OPUS;
            internalLibavSampleFmt = AV_SAMPLE_FMT_S16;
            outputStreamInfo->audio.sampleFormat = S16;
            break;
        case AAC:
            codecId = AV_CODEC_ID_AAC;
            internalLibavSampleFmt = AV_SAMPLE_FMT_S16;
            outputStreamInfo->audio.sampleFormat = S16;
            break;
        case MP3:
            codecId = AV_CODEC_ID_MP3;
            internalLibavSampleFmt = AV_SAMPLE_FMT_S16P;
            outputStreamInfo->audio.sampleFormat = S16P;
            break;
        default:
            codecId = AV_CODEC_ID_NONE;
            break;
    }

    return codingConfig(codecId);
}

bool AudioEncoderLibav::codingConfig(AVCodecID codecId) 
{
    codec = avcodec_find_encoder(codecId);
    if (!codec) {
        utils::errorMsg("Error finding encoder");
        return false;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (codecCtx == NULL) {
        utils::errorMsg("Error allocating codec context");
        return false;
    }

    if (getCodec() != PCMU && getCodec() != PCM) {
        if (!checkSampleFormat(codec, internalLibavSampleFmt)) {
            utils::errorMsg("Encoder does not support sample format");
            return false;
        }

        if (!checkSampleRateSupport(codec, outputStreamInfo->audio.sampleRate)) {
            utils::errorMsg("Encoder does not support sample rate " + std::to_string(outputStreamInfo->audio.sampleRate));
            return false;
        }

        if (!checkChannelLayoutSupport(codec, av_get_default_channel_layout(outputStreamInfo->audio.channels))) {
            utils::errorMsg("Encoder does not support channel layout");
            return false;
        }
    }

    codecCtx->channels = outputStreamInfo->audio.channels;
    codecCtx->channel_layout = av_get_default_channel_layout(outputStreamInfo->audio.channels);
    codecCtx->sample_rate = outputStreamInfo->audio.sampleRate;
    codecCtx->sample_fmt = internalLibavSampleFmt;
    codecCtx->bit_rate = outputBitrate;

    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        utils::errorMsg("Could not open codec context");
        return false;
    }

    if (codecCtx->frame_size != 0) {
        libavFrame->nb_samples = codecCtx->frame_size;
    } else {
        libavFrame->nb_samples = AudioFrame::getDefaultSamples(inputSampleRate);
    }

    libavFrame->format = codecCtx->sample_fmt;
    libavFrame->channel_layout = codecCtx->channel_layout;
    libavFrame->channels = outputStreamInfo->audio.channels;

    samplesPerFrame = libavFrame->nb_samples;

    if (av_frame_get_buffer(libavFrame, 0) < 0) {
        utils::errorMsg("Could not setup audio frame");
        return false;
    }

    return true;
}

bool AudioEncoderLibav::resamplingConfig()
{
    resampleCtx = swr_alloc_set_opts
                  (
                    resampleCtx,
                    av_get_default_channel_layout(outputStreamInfo->audio.channels),
                    internalLibavSampleFmt,
                    outputStreamInfo->audio.sampleRate,
                    av_get_default_channel_layout(inputChannels),
                    inputLibavSampleFmt,
                    inputSampleRate,
                    0,
                    NULL
                  );

    if (resampleCtx == NULL) {
        utils::errorMsg("Error allocating resample context");
        return false;
    }

    if (swr_is_initialized(resampleCtx) == 0) {
        if (swr_init(resampleCtx) < 0) {
            utils::errorMsg("Error initializing encoder resample context");
            return false;
        }
    }

    return true;
}

bool AudioEncoderLibav::reconfigure(AudioFrame* frame)
{
    if (inputSampleFmt != frame->getSampleFmt() || 
        inputChannels != frame->getChannels() ||
        inputSampleRate != frame->getSampleRate())
    {
        inputSampleFmt = frame->getSampleFmt();
        inputChannels = frame->getChannels();
        inputSampleRate = frame->getSampleRate();

        switch(inputSampleFmt) {
            case U8:
                inputLibavSampleFmt = AV_SAMPLE_FMT_U8;
                break;
            case S16:
                inputLibavSampleFmt = AV_SAMPLE_FMT_S16;
                break;
            case FLT:
                inputLibavSampleFmt = AV_SAMPLE_FMT_FLT;
                break;
            case U8P:
                inputLibavSampleFmt = AV_SAMPLE_FMT_U8P;
                break;
            case S16P:
                inputLibavSampleFmt = AV_SAMPLE_FMT_S16P;
                break;
            case FLTP:
                inputLibavSampleFmt = AV_SAMPLE_FMT_FLTP;
                break;
            default:
                inputLibavSampleFmt = AV_SAMPLE_FMT_NONE;
                break;
        }

        return resamplingConfig();
    }

    return true;
}

int AudioEncoderLibav::resample(AudioFrame* src, AVFrame* dst)
{
    int samples;
    unsigned char *auxBuff;

    if (src->isPlanar()) {
        samples = swr_convert(
                    resampleCtx,
                    dst->data,
                    dst->nb_samples,
                    (const uint8_t**)src->getPlanarDataBuf(),
                    src->getSamples()
                  );

    } else {
        auxBuff = src->getDataBuf();

        samples = swr_convert(
                    resampleCtx,
                    dst->data,
                    dst->nb_samples,
                    (const uint8_t**)auxBuff,
                    src->getSamples()
                  );

    }

    return samples;
}

void AudioEncoderLibav::doGetState(Jzon::Object &filterNode)
{
    filterNode.Add("codec", utils::getAudioCodecAsString(getCodec()));
    filterNode.Add("sampleRate", (int)outputStreamInfo->audio.sampleRate);
    filterNode.Add("channels", (int)outputStreamInfo->audio.channels);
}

bool checkSampleFormat(AVCodec *codec, enum AVSampleFormat sampleFmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sampleFmt) {
            return true;
        }
        p++;
    }

    return false;
}

bool checkSampleRateSupport(AVCodec *codec, int sampleRate)
{
    const int *p;

    if (!codec->supported_samplerates) {
        return false;
    }

    p = codec->supported_samplerates;
    while (*p) {
        if (*p == sampleRate) {
            return true;
        }
        p++;
    }

    return false;
}

bool checkChannelLayoutSupport(AVCodec *codec, uint64_t channelLayout)
{
    const uint64_t *p;

    if (!codec->channel_layouts)
        return false;

    p = codec->channel_layouts;
    while (*p) {
        if (*p == channelLayout) {
            return true;
        }
        p++;
    }

    return false;
}

bool AudioEncoderLibav::configEvent(Jzon::Node* params)
{
    ACodecType codec; 
    int codedAudioChannels;
    int codedAudioSampleRate;
    int bitrate;

    if (!params) {
        return false;
    }

    codec = getCodec();
    codedAudioChannels = outputStreamInfo->audio.channels;
    codedAudioSampleRate = outputStreamInfo->audio.sampleRate;
    bitrate = outputBitrate;

    if (params->Has("codec")) {
        codec = utils::getAudioCodecFromString(params->Get("codec").ToString());
    }

    if (params->Has("sampleRate")) {
        codedAudioSampleRate = params->Get("sampleRate").ToInt();
    }

    if (params->Has("channels")) {
        codedAudioChannels = params->Get("channels").ToInt();
    }

    if (params->Has("bitrate")) {
        bitrate = params->Get("bitrate").ToInt();
    }

    return configure0(codec, codedAudioChannels, codedAudioSampleRate, bitrate);
}

void AudioEncoderLibav::initializeEventMap()
{
    eventMap["configure"] = std::bind(&AudioEncoderLibav::configEvent, this, std::placeholders::_1);
}

bool AudioEncoderLibav::configure(ACodecType codec, int codedAudioChannels, int codedAudioSampleRate, int bitrate)
{
    Jzon::Object root, params;
    root.Add("action", "configure");
    params.Add("codec", utils::getAudioCodecAsString(codec));
    params.Add("channels", codedAudioChannels);
    params.Add("sampleRate", codedAudioSampleRate);
    params.Add("bitrate", bitrate);
    root.Add("params", params);

    Event e(root, std::chrono::system_clock::now(), 0);
    pushEvent(e); 
    return true;
}

