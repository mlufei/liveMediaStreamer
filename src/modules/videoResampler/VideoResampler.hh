/*
 *  VideoResampler.hh - A libav-based video resampler
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
 *  Authors: David Cassany <david.cassany@i2cat.net>  
 *           Marc Palau <marc.palau@i2cat.net>
 */

#ifndef _VIDEO_RESAMPLER_HH
#define _VIDEO_RESAMPLER_HH

extern "C" {
    #include <libswscale/swscale.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
}

#include "../../VideoFrame.hh"
#include "../../FrameQueue.hh"
#include "../../Filter.hh"
#include "../../StreamInfo.hh"

class VideoResampler : public OneToOneFilter {

    public:
        VideoResampler();
        ~VideoResampler();
        bool configure(int width, int height, int period, PixType pixelFormat);
        
    private:
        bool configure0(int width, int height, int period, PixType pixelFormat);
        bool doProcessFrame(Frame *org, Frame *dst);
        FrameQueue* allocQueue(ConnectionData cData);
        void initializeEventMap();
        bool configEvent(Jzon::Node* params);
        void doGetState(Jzon::Object &filterNode);
        bool reconfigure(VideoFrame* orgFrame);
        bool setAVFrame(AVFrame *aFrame, VideoFrame* vFrame, AVPixelFormat format);
        
        //NOTE: There is no need of specific reader configuration
        bool specificReaderConfig(int /*readerID*/, FrameQueue* /*queue*/)  {return true;};
        bool specificReaderDelete(int /*readerID*/) {return true;};
        
        //NOTE: There is no need of specific writer configuration
        bool specificWriterConfig(int /*writerID*/) {return true;};
        bool specificWriterDelete(int /*writerID*/) {return true;};
        
        struct SwsContext   *imgConvertCtx;
        AVFrame             *inFrame, *outFrame;
        AVPixelFormat       libavInPixFmt, libavOutPixFmt;

        StreamInfo          *outputStreamInfo;

        int                 outputWidth;
        int                 outputHeight;
        int                 discartCount;
        int                 discartPeriod;
        PixType             inPixFmt, outPixFmt;
        bool                needsConfig;
};

#endif

