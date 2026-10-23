// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "archive_frame_extractor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>

#include <core/resource/avi/avi_archive_delegate.h>
#include <core/resource/avi/avi_resource.h>
#include <core/resource/camera_resource.h>
#include <core/resource/media_resource.h>
#include <decoders/video/ffmpeg_video_decoder.h>
#include <nx/media/ffmpeg/frame_info.h>
#include <nx/media/media_data_packet.h>
#include <nx/streaming/abstract_archive_delegate.h>
#include <nx/streaming/rtsp_client_archive_delegate.h>
#include <nx/utils/log/assert.h>
#include <nx/utils/log/log.h>
#include <nx/vms/client/core/network/remote_connection.h>
#include <nx/vms/client/desktop/resource/local_camera/local_camera_archive_delegate.h>
#include <nx/vms/client/desktop/resource/local_camera/local_camera_resource.h>
#include <nx/vms/client/desktop/system_context.h>
#include <utils/common/delayed.h>

using namespace std::chrono;

namespace nx::vms::client::desktop {

namespace {

static constexpr auto kIdleStreamKeepAliveTime = 15s;
static constexpr auto kAcceptableTimeDivergence = 0.1;

milliseconds frameTimeMs(const QnCompressedVideoDataPtr& encodedFrame)
{
    if (!NX_ASSERT(encodedFrame))
        return 0ms;

    return round<milliseconds>(
        microseconds(encodedFrame->timestamp));
}

bool timeSatisfiesRequest(
    const ArchiveFrameExtractor::Request& request,
    milliseconds timePoint)
{
    return timePoint >= request.timePoint - request.tolerance
        && timePoint < request.timePoint + request.tolerance;
}

double requestedTimeDivergence(
    const ArchiveFrameExtractor::Request& request,
    milliseconds timePoint)
{
    if (request.tolerance == 0ms)
        return 0.0;
    return std::fabs(request.timePoint.count() - timePoint.count())
        / request.tolerance.count();
}

bool isKeyFrame(const QnCompressedVideoDataPtr& encodedFrame)
{
    return encodedFrame->flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey);
}

} // namespace

struct ArchiveFrameExtractor::Private
{
    ArchiveFrameExtractor* const q;

    std::atomic_bool stop = false;

    struct StreamWorker
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_ptr<std::thread> thread;

        std::deque<Request> requestQueue;

        /**
         * Resource the delegate has to be opened with, when that is up to this class rather than to
         * the delegate itself. Opened by the stream worker; null once that has been done.
         */
        QnMediaResourcePtr resourceToOpen;

        std::unique_ptr<QnAbstractArchiveDelegate> archiveDelegate;
        QnCompressedVideoDataPtr currentFrame;

        std::optional<microseconds> lastKeyFrameTime;
        std::optional<milliseconds> lastSeekTime;
        std::optional<microseconds> lastGopDuration;
    };
    StreamWorker streamWorker;

    struct DecodeJob
    {
        Request request;
        QSize scaleSize;
        std::vector<QnCompressedVideoDataPtr> frameSequence;
    };

    struct DecodeWorker
    {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::unique_ptr<std::thread> thread;

        std::deque<DecodeJob> decodeQueue;

        QSize scaleSize;
    };
    DecodeWorker decodeWorker;

    void returnResult(
        const Request& request,
        const CLVideoDecoderOutputPtr& decodedFrame,
        ResultCode resultCode)
    {
        Result result;
        result.request = request;
        result.decodedFrame = decodedFrame;
        result.resultCode = resultCode;
        executeLater([this, result]() { emit q->frameRequestDone(result); }, q);
    }

    void processRequests()
    {
        if (!streamWorker.resourceToOpen.isNull())
        {
            streamWorker.archiveDelegate->open(streamWorker.resourceToOpen);
            streamWorker.resourceToOpen.reset();
        }

        while (true)
        {
            std::unique_lock<std::mutex> lock(streamWorker.mutex);
            streamWorker.cv.wait_for(lock, kIdleStreamKeepAliveTime,
                [this] { return !streamWorker.requestQueue.empty() || stop; });

            if (stop)
                return;

            if (streamWorker.requestQueue.empty())
            {
                const auto canSeekImmediatly = streamWorker.archiveDelegate->getFlags().testFlag(
                    QnAbstractArchiveDelegate::Flag_CanSeekImmediatly);

                if (canSeekImmediatly && dynamic_cast<QnRtspClientArchiveDelegate*>(
                    streamWorker.archiveDelegate.get()))
                {
                    streamWorker.archiveDelegate->close();
                }

                lock.unlock();
                continue;
            }

            auto request = streamWorker.requestQueue.front();
            streamWorker.requestQueue.pop_front();
            lock.unlock();

            processRequest(request);
        }
    };

    void processRequest(const Request& request)
    {
        if (streamWorker.lastSeekTime && streamWorker.lastKeyFrameTime
            && request.timePoint > streamWorker.lastSeekTime.value()
            && (streamWorker.lastKeyFrameTime.value() > request.timePoint + request.tolerance))
        {
            returnResult(request, {}, ResultCode::noData);
            return;
        }

        bool seekIFrame = true;

        if (streamWorker.currentFrame)
        {
            const auto currentFrameTime = frameTimeMs(streamWorker.currentFrame);
            if (request.timePoint > currentFrameTime)
            {
                if ((currentFrameTime - streamWorker.lastKeyFrameTime.value())
                    > (request.timePoint - currentFrameTime))
                {
                    seekIFrame = false;
                }

                if (streamWorker.lastGopDuration
                    && (request.timePoint - streamWorker.lastKeyFrameTime.value())
                        > streamWorker.lastGopDuration.value())
                {
                    seekIFrame = true;
                }
            }
        }

        if (seekIFrame)
        {
            streamWorker.lastSeekTime = request.timePoint;
            const qint64 seekResult = streamWorker.archiveDelegate->seek(
                duration_cast<microseconds>(request.timePoint).count(),
                /*findIFrame*/ true);

            streamWorker.currentFrame = getNextVideoData();

            NX_DEBUG(q, "Frame request at %1 ms: seek result %2, first frame at %3 ms",
                request.timePoint.count(), seekResult,
                streamWorker.currentFrame
                    ? frameTimeMs(streamWorker.currentFrame).count()
                    : -1);
            if (!streamWorker.currentFrame)
            {
                returnResult(request, {}, ResultCode::endReached);
                streamWorker.lastKeyFrameTime.reset();
                return;
            }
            streamWorker.lastKeyFrameTime =
                microseconds(streamWorker.currentFrame->timestamp);
        }
        else
        {
            streamWorker.currentFrame = getNextVideoData();
            if (!streamWorker.currentFrame)
            {
                returnResult(request, {}, ResultCode::endReached);
                streamWorker.lastKeyFrameTime.reset();
                return;
            }
            else
            {
                if (isKeyFrame(streamWorker.currentFrame))
                {
                    streamWorker.lastKeyFrameTime =
                        microseconds(streamWorker.currentFrame->timestamp);
                }
            }
        }

        if (streamWorker.lastKeyFrameTime.value() > request.timePoint + request.tolerance)
        {
            returnResult(request, {}, ResultCode::noData);
            return;
        }

        DecodeJob decodeJob({request});

        if (streamWorker.currentFrame)
            decodeJob.frameSequence.push_back(streamWorker.currentFrame);

        while (streamWorker.currentFrame
            && requestedTimeDivergence(request, frameTimeMs(streamWorker.currentFrame))
                > kAcceptableTimeDivergence
            && frameTimeMs(streamWorker.currentFrame) <= request.timePoint)
        {
            if (stop)
                return;

            streamWorker.currentFrame = getNextVideoData();
            if (streamWorker.currentFrame)
            {
                if (isKeyFrame(streamWorker.currentFrame))
                {
                    streamWorker.lastGopDuration =
                        microseconds(streamWorker.currentFrame->timestamp)
                            - streamWorker.lastKeyFrameTime.value();
                    decodeJob.frameSequence.clear();
                    streamWorker.lastKeyFrameTime =
                        microseconds(streamWorker.currentFrame->timestamp);
                }
                decodeJob.frameSequence.push_back(streamWorker.currentFrame);
            }
            else
            {
                streamWorker.lastKeyFrameTime.reset();
            }
        }

        if (decodeJob.frameSequence.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(decodeWorker.mutex);
            decodeJob.scaleSize = decodeWorker.scaleSize;
            decodeWorker.decodeQueue.push_back(decodeJob);
        }
        decodeWorker.cv.notify_one();
    }

    QnCompressedVideoDataPtr getNextVideoData()
    {
        QnCompressedVideoDataPtr compressedVideoData;
        while (!compressedVideoData)
        {
            auto mediaData = streamWorker.archiveDelegate->getNextData();
            if (!mediaData || mediaData->dataType == QnAbstractMediaData::EMPTY_DATA)
                return {}; //< End found.

            if (auto metaData = std::dynamic_pointer_cast<QnCompressedMetadata>(mediaData))
            {
                if (metaData->metadataType == MetadataType::MediaStreamEvent)
                    return {};
            }

            compressedVideoData = std::dynamic_pointer_cast<QnCompressedVideoData>(mediaData);
        }
        return compressedVideoData;
    };

    void decodeFrame()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(decodeWorker.mutex);
            decodeWorker.cv.wait(lock, [this] { return !decodeWorker.decodeQueue.empty() || stop; });

            if (stop)
                return;

            auto decodeJob = decodeWorker.decodeQueue.front();
            decodeWorker.decodeQueue.pop_front();

            lock.unlock();

            auto decoder = std::make_unique<QnFfmpegVideoDecoder>(
                DecoderConfig(), /*metrics*/ nullptr, decodeJob.frameSequence.front());

            CLVideoDecoderOutputPtr decodedFrame;
            for (const auto& compressedData: decodeJob.frameSequence)
            {
                if (stop.load())
                    return;
                CLVideoDecoderOutputPtr frame(new CLVideoDecoderOutput());
                if (decoder->decode(compressedData, &frame))
                    decodedFrame = frame;
            }
            {
                // Flushing data.
                CLVideoDecoderOutputPtr frame(new CLVideoDecoderOutput());
                while (decoder->decode(QnCompressedVideoDataPtr(), &frame))
                {
                    decodedFrame = frame;
                    frame.reset(new CLVideoDecoderOutput());
                }
            }

            if (decodedFrame && !decodeJob.scaleSize.isEmpty())
                decodedFrame = decodedFrame->scaled(decodeJob.scaleSize);

            const ResultCode resultCode = decodedFrame.isNull()
                ? ResultCode::decodeFailed
                : ResultCode::decoded;

            NX_DEBUG(q, "Request at %1 ms: decoded %2 packet(s), result code %3",
                decodeJob.request.timePoint.count(), decodeJob.frameSequence.size(),
                static_cast<int>(resultCode));

            returnResult(decodeJob.request, decodedFrame, resultCode);
        }
    };
};

ArchiveFrameExtractor::ArchiveFrameExtractor(
    const QnMediaResourcePtr& mediaResource,
    VideoQuality videQuality,
    bool sleepIfEmptySocket)
    :
    base_type(),
    d(new Private{this})
{
    if (!NX_ASSERT(!mediaResource.isNull()))
        return;

    if (!NX_ASSERT(mediaResource->hasVideo()))
        return;

    if (const auto cameraResource = mediaResource.dynamicCast<QnVirtualCameraResource>())
    {
        auto context = SystemContext::fromResource(cameraResource);
        if (!NX_ASSERT(context))
            return;

        auto rtspDelegate = std::make_unique<QnRtspClientArchiveDelegate>(
            /*archiveStreamReader*/ nullptr,
            "ArchiveFrameExtractor",
            sleepIfEmptySocket);
        rtspDelegate->setCamera(cameraResource);
        rtspDelegate->setMediaRole(PlaybackMode::archive);
        d->streamWorker.archiveDelegate = std::move(rtspDelegate);
    }
    else if (const auto aviResource = mediaResource.dynamicCast<QnAviResource>())
    {
        d->streamWorker.archiveDelegate.reset(aviResource->createArchiveDelegate());
    }
    else if (const auto localCamera = mediaResource.dynamicCast<LocalCameraResource>())
    {
        // A client-side source is either a live stream or a finite file. Only a file is worth
        // extracting thumbnails from, and opening a live one here would connect to it a second
        // time, competing with the playback that is already running.
        if (localCamera->isSeekableFileSource())
        {
            d->streamWorker.archiveDelegate = std::make_unique<LocalCameraArchiveDelegate>(
                localCamera->deviceId(), localCamera->url(), /*filePlaybackExpected*/ true);
        }
    }

    if (!d->streamWorker.archiveDelegate)
    {
        // Some media resources, e.g. live client-side cameras, provide neither an RTSP backend nor a
        // local file, so there is nothing to extract frames from.
        NX_WARNING(this, "Unable to create an archive delegate for resource '%1', "
            "frame extraction is not available.", mediaResource->getName());
        return;
    }

    static constexpr auto kAutoSize = QSize(-1, 0);
    const MediaQuality mediaQuality = videQuality == VideoQuality::low
        ? MEDIA_Quality_Low
        : MEDIA_Quality_High;
    d->streamWorker.archiveDelegate->setQuality(mediaQuality, /*fastSwitch*/ false, kAutoSize);

    if (!d->streamWorker.archiveDelegate->getFlags().testFlag(
        QnAbstractArchiveDelegate::Flag_CanSeekImmediatly))
    {
        // Opening may have to fetch the media, so it is left to the stream worker: the interface is
        // not held up by it, and the frames appear once the source is open.
        d->streamWorker.resourceToOpen = mediaResource;
    }

    d->streamWorker.thread = std::make_unique<std::thread>(&Private::processRequests, d.get());
    d->decodeWorker.thread = std::make_unique<std::thread>(&Private::decodeFrame, d.get());
}

ArchiveFrameExtractor::~ArchiveFrameExtractor()
{
    d->stop.store(true);

    // The stream worker may be inside a long open() of its own, so it is asked to give up before it is
    // waited for.
    if (d->streamWorker.archiveDelegate)
        d->streamWorker.archiveDelegate->pleaseStop();

    d->streamWorker.cv.notify_one();
    d->decodeWorker.cv.notify_one();

    if (d->streamWorker.thread)
        d->streamWorker.thread->join();

    if (d->decodeWorker.thread)
        d->decodeWorker.thread->join();
}

QSize ArchiveFrameExtractor::scaleSize()
{
    std::lock_guard<std::mutex> lock(d->decodeWorker.mutex);
    return d->decodeWorker.scaleSize;
}

void ArchiveFrameExtractor::setScaleSize(const QSize& size)
{
    std::lock_guard<std::mutex> lock(d->streamWorker.mutex);
    d->decodeWorker.scaleSize = size;
}

void ArchiveFrameExtractor::requestFrame(const Request& request)
{
    if (!d->streamWorker.thread)
        return; //< Frame extraction is not available for this resource.

    {
        std::lock_guard<std::mutex> lock(d->streamWorker.mutex);
        d->streamWorker.requestQueue.push_back(request);
    }
    d->streamWorker.cv.notify_one();
}

void ArchiveFrameExtractor::clearRequestQueue()
{
    std::scoped_lock lock{d->streamWorker.mutex, d->decodeWorker.mutex};
    d->streamWorker.requestQueue.clear();
    d->decodeWorker.decodeQueue.clear();
}

} // namespace nx::vms::client::desktop
