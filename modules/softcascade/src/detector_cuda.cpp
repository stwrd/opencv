/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
// Copyright (C) 2008-2012, Willow Garage Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "precomp.hpp"

#if !defined (HAVE_CUDA)
#define throw_nogpu() CV_Error(CV_GpuNotSupported, "The library is compiled without CUDA support")
cv::softcascade::SCascade::SCascade(const double, const double, const int, const int) { throw_nogpu(); }

cv::softcascade::SCascade::~SCascade() { throw_nogpu(); }

bool cv::softcascade::SCascade::load(const FileNode&) { throw_nogpu(); return false;}

void cv::softcascade::SCascade::detect(InputArray, InputArray, OutputArray, cv::gpu::Stream&) const { throw_nogpu(); }

void cv::softcascade::SCascade::read(const FileNode& fn) { Algorithm::read(fn); }

cv::softcascade::ChannelsProcessor::ChannelsProcessor() { throw_nogpu(); }
 cv::softcascade::ChannelsProcessor::~ChannelsProcessor() { throw_nogpu(); }

cv::Ptr<cv::softcascade::ChannelsProcessor> cv::softcascade::ChannelsProcessor::create(const int, const int, const int)
{ throw_nogpu(); return cv::Ptr<cv::softcascade::ChannelsProcessor>(0); }

#else
# include "cuda_invoker.hpp"
# include "opencv2/core/stream_accessor.hpp"
namespace
{
#if defined(__GNUC__)
    #define cudaSafeCall(expr)  ___cudaSafeCall(expr, __FILE__, __LINE__, __func__)
#else /* defined(__CUDACC__) || defined(__MSVC__) */
    #define cudaSafeCall(expr)  ___cudaSafeCall(expr, __FILE__, __LINE__)
#endif

    inline void ___cudaSafeCall(cudaError_t err, const char *file, const int line, const char *func = "")
    {
        if (cudaSuccess != err) cv::gpu::error(cudaGetErrorString(err), file, line, func);
    }
}

cv::softcascade::device::Level::Level(int idx, const Octave& oct, const float scale, const int w, const int h)
:  octave(idx), step(oct.stages), relScale(scale / oct.scale)
{
    workRect.x = cvRound(w / (float)oct.shrinkage);
    workRect.y = cvRound(h / (float)oct.shrinkage);

    objSize.x  = cv::saturate_cast<uchar>(oct.size.x * relScale);
    objSize.y  = cv::saturate_cast<uchar>(oct.size.y * relScale);

    // according to R. Benenson, M. Mathias, R. Timofte and L. Van Gool's and Dallal's papers
    if (fabs(relScale - 1.f) < FLT_EPSILON)
        scaling[0] = scaling[1] = 1.f;
    else
    {
        scaling[0] = (relScale < 1.f) ? 0.89f * ::pow(relScale, 1.099f / ::log(2.0f)) : 1.f;
        scaling[1] = relScale * relScale;
    }
}

namespace cv { namespace softcascade { namespace device {

    void fillBins(cv::gpu::PtrStepSzb hogluv, const cv::gpu::PtrStepSzf& nangle,
        const int fw, const int fh, const int bins, cudaStream_t stream);

    void suppress(const cv::gpu::PtrStepSzb& objects, cv::gpu::PtrStepSzb overlaps, cv::gpu::PtrStepSzi ndetections,
        cv::gpu::PtrStepSzb suppressed, cudaStream_t stream);

    void bgr2Luv(const cv::gpu::PtrStepSzb& bgr, cv::gpu::PtrStepSzb luv);
    void transform(const cv::gpu::PtrStepSz<uchar3>& bgr, cv::gpu::PtrStepSzb gray);
    void gray2hog(const cv::gpu::PtrStepSzb& gray, cv::gpu::PtrStepSzb mag, const int bins);
    void shrink(const cv::gpu::PtrStepSzb& channels, cv::gpu::PtrStepSzb shrunk);

    void shfl_integral(const cv::gpu::PtrStepSzb& img, cv::gpu::PtrStepSz<unsigned int> integral, cudaStream_t stream);
}}}

struct cv::softcascade::SCascade::Fields
{
    static Fields* parseCascade(const FileNode &root, const float mins, const float maxs, const int totals, const int method)
    {
        static const char *const SC_STAGE_TYPE          = "stageType";
        static const char *const SC_BOOST               = "BOOST";
        static const char *const SC_FEATURE_TYPE        = "featureType";
        static const char *const SC_ICF                 = "ICF";
        static const char *const SC_ORIG_W              = "width";
        static const char *const SC_ORIG_H              = "height";
        static const char *const SC_FEATURE_FORMAT      = "featureFormat";
        static const char *const SC_SHRINKAGE           = "shrinkage";
        static const char *const SC_OCTAVES             = "octaves";
        static const char *const SC_OCT_SCALE           = "scale";
        static const char *const SC_OCT_WEAKS           = "weaks";
        static const char *const SC_TREES               = "trees";
        static const char *const SC_WEAK_THRESHOLD      = "treeThreshold";
        static const char *const SC_FEATURES            = "features";
        static const char *const SC_INTERNAL            = "internalNodes";
        static const char *const SC_LEAF                = "leafValues";
        static const char *const SC_F_CHANNEL           = "channel";
        static const char *const SC_F_RECT              = "rect";

        // only Ada Boost supported
        std::string stageTypeStr = (std::string)root[SC_STAGE_TYPE];
        CV_Assert(stageTypeStr == SC_BOOST);

        // only HOG-like integral channel features supported
        std::string featureTypeStr = (std::string)root[SC_FEATURE_TYPE];
        CV_Assert(featureTypeStr == SC_ICF);

        int origWidth  = (int)root[SC_ORIG_W];
        int origHeight = (int)root[SC_ORIG_H];

        std::string fformat = (std::string)root[SC_FEATURE_FORMAT];
        bool useBoxes = (fformat == "BOX");
        ushort shrinkage = cv::saturate_cast<ushort>((int)root[SC_SHRINKAGE]);

        FileNode fn = root[SC_OCTAVES];
        if (fn.empty()) return 0;

        std::vector<device::Octave>  voctaves;
        std::vector<float>   vstages;
        std::vector<device::Node>    vnodes;
        std::vector<float>   vleaves;

        FileNodeIterator it = fn.begin(), it_end = fn.end();
        for (ushort octIndex = 0; it != it_end; ++it, ++octIndex)
        {
            FileNode fns = *it;
            float scale = powf(2.f,saturate_cast<float>((int)fns[SC_OCT_SCALE]));
            bool isUPOctave = scale >= 1;

            ushort nweaks = saturate_cast<ushort>((int)fns[SC_OCT_WEAKS]);

            ushort2 size;
            size.x = cvRound(origWidth * scale);
            size.y = cvRound(origHeight * scale);

            device::Octave octave(octIndex, nweaks, shrinkage, size, scale);
            CV_Assert(octave.stages > 0);
            voctaves.push_back(octave);

            FileNode ffs = fns[SC_FEATURES];
            if (ffs.empty()) return 0;

            std::vector<cv::Rect> feature_rects;
            std::vector<int> feature_channels;

            FileNodeIterator ftrs = ffs.begin(), ftrs_end = ffs.end();
            int feature_offset = 0;
            for (; ftrs != ftrs_end; ++ftrs, ++feature_offset )
            {
                cv::FileNode ftn = (*ftrs)[SC_F_RECT];
                cv::FileNodeIterator r_it = ftn.begin();
                int x = (int)*(r_it++);
                int y = (int)*(r_it++);
                int w = (int)*(r_it++);
                int h = (int)*(r_it++);

                if (useBoxes)
                {
                    if (isUPOctave)
                    {
                        w -= x;
                        h -= y;
                    }
                }
                else
                {
                    if (!isUPOctave)
                    {
                        w += x;
                        h += y;
                    }
                }
                feature_rects.push_back(cv::Rect(x, y, w, h));
                feature_channels.push_back((int)(*ftrs)[SC_F_CHANNEL]);
            }

            fns = fns[SC_TREES];
            if (fn.empty()) return false;

            // for each stage (~ decision tree with H = 2)
            FileNodeIterator st = fns.begin(), st_end = fns.end();
            for (; st != st_end; ++st )
            {
                FileNode octfn = *st;
                float threshold = (float)octfn[SC_WEAK_THRESHOLD];
                vstages.push_back(threshold);

                FileNode intfns = octfn[SC_INTERNAL];
                FileNodeIterator inIt = intfns.begin(), inIt_end = intfns.end();
                for (; inIt != inIt_end;)
                {
                    inIt +=2;
                    int featureIdx = (int)(*(inIt++));

                    float orig_threshold = (float)(*(inIt++));
                    unsigned int th = saturate_cast<unsigned int>((int)orig_threshold);
                    cv::Rect& r = feature_rects[featureIdx];
                    uchar4 rect;
                    rect.x = saturate_cast<uchar>(r.x);
                    rect.y = saturate_cast<uchar>(r.y);
                    rect.z = saturate_cast<uchar>(r.width);
                    rect.w = saturate_cast<uchar>(r.height);

                    unsigned int channel = saturate_cast<unsigned int>(feature_channels[featureIdx]);
                    vnodes.push_back(device::Node(rect, channel, th));
                }

                intfns = octfn[SC_LEAF];
                inIt = intfns.begin(), inIt_end = intfns.end();
                for (; inIt != inIt_end; ++inIt)
                {
                    vleaves.push_back((float)(*inIt));
                }
            }
        }

        cv::Mat hoctaves(1, (int) (voctaves.size() * sizeof(device::Octave)), CV_8UC1, (uchar*)&(voctaves[0]));
        CV_Assert(!hoctaves.empty());

        cv::Mat hstages(cv::Mat(vstages).reshape(1,1));
        CV_Assert(!hstages.empty());

        cv::Mat hnodes(1, (int) (vnodes.size() * sizeof(device::Node)), CV_8UC1, (uchar*)&(vnodes[0]) );
        CV_Assert(!hnodes.empty());

        cv::Mat hleaves(cv::Mat(vleaves).reshape(1,1));
        CV_Assert(!hleaves.empty());

        Fields* fields = new Fields(mins, maxs, totals, origWidth, origHeight, shrinkage, 0,
            hoctaves, hstages, hnodes, hleaves, method);
        fields->voctaves = voctaves;
        fields->createLevels(DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_WIDTH);

        return fields;
    }

    bool check(float mins,float  maxs, int scales)
    {
        bool updated = (minScale == mins) || (maxScale == maxs) || (totals = scales);

        minScale = mins;
        maxScale = maxScale;
        totals   = scales;

        return updated;
    }

    int createLevels(const int fh, const int fw)
    {
        std::vector<device::Level> vlevels;
        float logFactor = (::log(maxScale) - ::log(minScale)) / (totals -1);

        float scale = minScale;
        int dcs = 0;
        for (int sc = 0; sc < totals; ++sc)
        {
            int width  = (int)::std::max(0.0f, fw - (origObjWidth  * scale));
            int height = (int)::std::max(0.0f, fh - (origObjHeight * scale));

            float logScale = ::log(scale);
            int fit = fitOctave(voctaves, logScale);

            device::Level level(fit, voctaves[fit], scale, width, height);

            if (!width || !height)
                break;
            else
            {
                vlevels.push_back(level);
                if (voctaves[fit].scale < 1) ++dcs;
            }

            if (::fabs(scale - maxScale) < FLT_EPSILON) break;
            scale = ::std::min(maxScale, ::expf(::log(scale) + logFactor));
        }

        cv::Mat hlevels = cv::Mat(1, (int) (vlevels.size() * sizeof(device::Level)), CV_8UC1, (uchar*)&(vlevels[0]) );
        CV_Assert(!hlevels.empty());
        levels.upload(hlevels);
        downscales = dcs;
        return dcs;
    }

    bool update(int fh, int fw, int shr)
    {
        shrunk.create(fh / shr * HOG_LUV_BINS, fw / shr, CV_8UC1);
        integralBuffer.create(shrunk.rows, shrunk.cols, CV_32SC1);

        hogluv.create((fh / shr) * HOG_LUV_BINS + 1, fw / shr + 1, CV_32SC1);
        hogluv.setTo(cv::Scalar::all(0));

        overlaps.create(1, 5000, CV_8UC1);
        suppressed.create(1, sizeof(Detection) * 51, CV_8UC1);

        return true;
    }

    Fields( const float mins, const float maxs, const int tts, const int ow, const int oh, const int shr, const int ds,
        cv::Mat hoctaves, cv::Mat hstages, cv::Mat hnodes, cv::Mat hleaves, int method)
    : minScale(mins), maxScale(maxs), totals(tts), origObjWidth(ow), origObjHeight(oh), shrinkage(shr), downscales(ds)
    {
        update(DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_WIDTH, shr);
        octaves.upload(hoctaves);
        stages.upload(hstages);
        nodes.upload(hnodes);
        leaves.upload(hleaves);

        preprocessor = ChannelsProcessor::create(shrinkage, 6, method);
    }

    void detect(cv::gpu::GpuMat& objects, cv::gpu::Stream& s) const
    {
        if (s)
            s.enqueueMemSet(objects, 0);
        else
            cudaMemset(objects.data, 0, sizeof(Detection));

        cudaSafeCall( cudaGetLastError());

        device::CascadeInvoker<device::GK107PolicyX4> invoker
        = device::CascadeInvoker<device::GK107PolicyX4>(levels, stages, nodes, leaves);

        cudaStream_t stream = cv::gpu::StreamAccessor::getStream(s);
        invoker(mask, hogluv, objects, downscales, stream);
    }

    void suppress(cv::gpu::GpuMat& objects, cv::gpu::Stream& s)
    {
        cv::gpu::GpuMat ndetections = cv::gpu::GpuMat(objects, cv::Rect(0, 0, sizeof(Detection), 1));
        ensureSizeIsEnough(objects.rows, objects.cols, CV_8UC1, overlaps);

        if (s)
        {
            s.enqueueMemSet(overlaps, 0);
            s.enqueueMemSet(suppressed, 0);
        }
        else
        {
            overlaps.setTo(0);
            suppressed.setTo(0);
        }

        cudaStream_t stream = cv::gpu::StreamAccessor::getStream(s);
        device::suppress(objects, overlaps, ndetections, suppressed, stream);
    }

private:

    typedef std::vector<device::Octave>::const_iterator  octIt_t;
    static int fitOctave(const std::vector<device::Octave>& octs, const float& logFactor)
    {
        float minAbsLog = FLT_MAX;
        int res =  0;
        for (int oct = 0; oct < (int)octs.size(); ++oct)
        {
            const device::Octave& octave =octs[oct];
            float logOctave = ::log(octave.scale);
            float logAbsScale = ::fabs(logFactor - logOctave);

            if(logAbsScale < minAbsLog)
            {
                res = oct;
                minAbsLog = logAbsScale;
            }
        }
        return res;
    }

public:

    cv::Ptr<ChannelsProcessor> preprocessor;

    // scales range
    float minScale;
    float maxScale;

    int totals;

    int origObjWidth;
    int origObjHeight;

    const int shrinkage;
    int downscales;


    // 160x120x10
    cv::gpu::GpuMat shrunk;

    // temporal mat for integral
    cv::gpu::GpuMat integralBuffer;

    // 161x121x10
    cv::gpu::GpuMat hogluv;


    // used for suppression
    cv::gpu::GpuMat suppressed;
    // used for area overlap computing during
    cv::gpu::GpuMat overlaps;


    // Cascade from xml
    cv::gpu::GpuMat octaves;
    cv::gpu::GpuMat stages;
    cv::gpu::GpuMat nodes;
    cv::gpu::GpuMat leaves;
    cv::gpu::GpuMat levels;


    // For ROI
    cv::gpu::GpuMat mask;
    cv::gpu::GpuMat genRoiTmp;

//     cv::gpu::GpuMat collected;


    std::vector<device::Octave> voctaves;

//     DeviceInfo info;

    enum { BOOST = 0 };
    enum
    {
        DEFAULT_FRAME_WIDTH        = 640,
        DEFAULT_FRAME_HEIGHT       = 480,
        HOG_LUV_BINS               = 10
    };
};

cv::softcascade::SCascade::SCascade(const double mins, const double maxs, const int sc, const int fl)
: fields(0),  minScale(mins), maxScale(maxs), scales(sc), flags(fl) {}

cv::softcascade::SCascade::~SCascade() { delete fields; }

bool cv::softcascade::SCascade::load(const FileNode& fn)
{
    if (fields) delete fields;
    fields = Fields::parseCascade(fn, (float)minScale, (float)maxScale, scales, flags);
    return fields != 0;
}

namespace {

void integral(const cv::gpu::GpuMat& src, cv::gpu::GpuMat& sum, cv::gpu::GpuMat& buffer, cv::gpu::Stream& s)
{
    CV_Assert(src.type() == CV_8UC1);

    cudaStream_t stream = cv::gpu::StreamAccessor::getStream(s);

    cv::Size whole;
    cv::Point offset;

    src.locateROI(whole, offset);

    if (cv::gpu::deviceSupports(cv::gpu::WARP_SHUFFLE_FUNCTIONS) && src.cols <= 2048
        && offset.x % 16 == 0 && ((src.cols + 63) / 64) * 64 <= (static_cast<int>(src.step) - offset.x))
    {
        ensureSizeIsEnough(((src.rows + 7) / 8) * 8, ((src.cols + 63) / 64) * 64, CV_32SC1, buffer);

        cv::softcascade::device::shfl_integral(src, buffer, stream);

        sum.create(src.rows + 1, src.cols + 1, CV_32SC1);
        if (s)
            s.enqueueMemSet(sum, cv::Scalar::all(0));
        else
            sum.setTo(cv::Scalar::all(0));

        cv::gpu::GpuMat inner = sum(cv::Rect(1, 1, src.cols, src.rows));
        cv::gpu::GpuMat res = buffer(cv::Rect(0, 0, src.cols, src.rows));

        if (s)
            s.enqueueCopy(res, inner);
        else
            res.copyTo(inner);
    }
    else {CV_Error(CV_GpuNotSupported, ": CC 3.x required.");}
}

}

void cv::softcascade::SCascade::detect(InputArray _image, InputArray _rois, OutputArray _objects, cv::gpu::Stream& s) const
{
    CV_Assert(fields);

    // only color images and precomputed integrals are supported
    int type = _image.type();
    CV_Assert(type == CV_8UC3 || type == CV_32SC1 || (!_rois.empty()));

    const cv::gpu::GpuMat image = _image.getGpuMat();

    if (_objects.empty()) _objects.create(1, 4096 * sizeof(Detection), CV_8UC1);

    cv::gpu::GpuMat rois = _rois.getGpuMat(), objects = _objects.getGpuMat();

    /// roi
    Fields& flds = *fields;
    int shr = flds.shrinkage;

    flds.mask.create( rois.cols / shr, rois.rows / shr, rois.type());

    device::shrink(rois, flds.mask);
    //cv::gpu::transpose(flds.genRoiTmp, flds.mask, s);

    if (type == CV_8UC3)
    {
        flds.update(image.rows, image.cols, flds.shrinkage);

        if (flds.check((float)minScale, (float)maxScale, scales))
            flds.createLevels(image.rows, image.cols);

        flds.preprocessor->apply(image, flds.shrunk);
        integral(flds.shrunk, flds.hogluv, flds.integralBuffer, s);
    }
    else
    {
        if (s)
            s.enqueueCopy(image, flds.hogluv);
        else
            image.copyTo(flds.hogluv);
    }

    flds.detect(objects, s);

    if ( (flags && NMS_MASK) != NO_REJECT)
    {
        cv::gpu::GpuMat spr(objects, cv::Rect(0, 0, flds.suppressed.cols, flds.suppressed.rows));
        flds.suppress(objects, s);
        flds.suppressed.copyTo(spr);
    }
}

void cv::softcascade::SCascade::read(const FileNode& fn)
{
    Algorithm::read(fn);
}

namespace {

using cv::InputArray;
using cv::OutputArray;
using cv::gpu::Stream;
using cv::gpu::GpuMat;

inline void setZero(cv::gpu::GpuMat& m, cv::gpu::Stream& s)
{
    if (s)
        s.enqueueMemSet(m, 0);
    else
        m.setTo(0);
}

struct SeparablePreprocessor : public cv::softcascade::ChannelsProcessor
{
    SeparablePreprocessor(const int s, const int b) : cv::softcascade::ChannelsProcessor(), shrinkage(s), bins(b) {}
    virtual ~SeparablePreprocessor() {}

    virtual void apply(InputArray _frame, OutputArray _shrunk, cv::gpu::Stream& s = cv::gpu::Stream::Null())
    {
        bgr = _frame.getGpuMat();
        //cv::gpu::GaussianBlur(frame, bgr, cv::Size(3, 3), -1.0);

        _shrunk.create(bgr.rows * (4 + bins) / shrinkage, bgr.cols / shrinkage, CV_8UC1);
        cv::gpu::GpuMat shrunk = _shrunk.getGpuMat();

        channels.create(bgr.rows * (4 + bins), bgr.cols, CV_8UC1);
        setZero(channels, s);

        gray.create(bgr.size(), CV_8UC1);
        cv::softcascade::device::transform(bgr, gray); //cv::gpu::cvtColor(bgr, gray, CV_BGR2GRAY);
        cv::softcascade::device::gray2hog(gray, channels(cv::Rect(0, 0, bgr.cols, bgr.rows * (bins + 1))), bins);

        cv::gpu::GpuMat luv(channels, cv::Rect(0, bgr.rows * (bins + 1), bgr.cols, bgr.rows * 3));
        cv::softcascade::device::bgr2Luv(bgr, luv);
        cv::softcascade::device::shrink(channels, shrunk);
    }

private:
    const int shrinkage;
    const int bins;

    cv::gpu::GpuMat bgr;
    cv::gpu::GpuMat gray;
    cv::gpu::GpuMat channels;
};

}

cv::Ptr<cv::softcascade::ChannelsProcessor> cv::softcascade::ChannelsProcessor::create(const int s, const int b, const int m)
{
    CV_Assert((m && SEPARABLE));
    return cv::Ptr<cv::softcascade::ChannelsProcessor>(new SeparablePreprocessor(s, b));
}

cv::softcascade::ChannelsProcessor::ChannelsProcessor() { }
cv::softcascade::ChannelsProcessor::~ChannelsProcessor() { }

#endif
