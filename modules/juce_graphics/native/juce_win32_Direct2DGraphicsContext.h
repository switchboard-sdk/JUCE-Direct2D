/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 7 End-User License
   Agreement and JUCE Privacy Policy.

   End User License Agreement: www.juce.com/juce-7-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

#ifndef _WINDEF_
class HWND__; // Forward or never
typedef HWND__* HWND;
#endif

#if JUCE_DIRECT2D_METRICS

namespace direct2d
{
    struct PaintEvent
    {
        PaintEvent(int code_, char const* const name_) :
            code(code_),
            name(name_)
        {
        }

        int code;
        char const* const name;
        int64 startTicks = Time::getHighResolutionTicks();
        int64 finishTicks;
        
        double getDurationMsec() const
        {
            auto elapsed = finishTicks - startTicks;
            return Time::highResolutionTicksToSeconds(elapsed) * 1000.0;
        }
        
        enum
        {
            setOrigin,
            addTransform,
            clipToRectangle,
            clipToRectangleList,
            excludeClipRectangle,
            clipToPath,
            clipToImageAlpha,
            saveState,
            restoreState,
            fillRect,
            fillRectList,
            drawRect,
            beginTransparencyLayer,
            setFill,
            setOpacity,
            setInterpolationQuality,
            fillPath,
            drawPath,
            drawImage,
            drawLine,
            setFont,
            drawGlyph,
            drawTextLayout,
            drawGlyphRun,
            drawRoundedRectangle,
            fillRoundedRectangle,
            drawEllipse,
            fillEllipse
        };
    };

    struct Frame
    {
        int frameNumber;
        int64 frameStartTicks = Time::getHighResolutionTicks();
        int64 frameFinishTicks = 0;
        RectangleList<int> rects;
        Array<PaintEvent> events;

        void addEvent(PaintEvent&& event)
        {
            events.add(event);
        }

        PaintEvent& getMostRecentEvent()
        {
            return events.getReference(events.size() - 1);
        }

        double getDurationMsec() const
        {
            auto elapsed = frameFinishTicks - frameStartTicks;
            return Time::highResolutionTicksToSeconds(elapsed) * 1000.0;
        }
    };

    struct PaintStats : public ReferenceCountedObject
    {
        enum
        {
            paintDuration,
            threadPaintDuration,
            presentDuration,
            numStats
        };

        static constexpr int maxEvents = 65536;
        static constexpr int maxFrames = 1024;

        StatisticsAccumulator<double> accumulators[numStats];
        int64 const creationTime = Time::getMillisecondCounter();
        double const millisecondsPerTick = 1000.0 / (double)Time::getHighResolutionTicksPerSecond();
        int paintCount = 0;
        int presentCount = 0;
        int64 lastPaintStartTicks = 0;
        uint64 lockAcquireMaxTicks = 0;

        std::deque<Frame> frames;

        void startFrame(int frameNumber)
        {
            while (frames.size() > maxFrames)
            {
                frames.pop_back();
            }

            if (frames.size() > 0)
            {
                if (auto& lastFrame = frames.front(); lastFrame.frameFinishTicks == 0)
                {
                    frames.pop_front();
                }
            }

            frames.push_front(Frame{ frameNumber });
        }

        void finishFrame()
        {
            getMostRecentFrame().frameFinishTicks = Time::getHighResolutionTicks();
        }

        Frame& getMostRecentFrame()
        {
            jassert(frames.size() > 0);
            return frames.front();
        }

        PaintStats()
        {
        }

        ~PaintStats()
        {
#if 0
            for (auto frame : frames)
            {
                DBG("\nFrame #" << frame.frameNumber << " " << (frame.finishTicks - frame.startTicks) * millisecondsPerTick << " ms");
                for (int i = frame.firstEventIndex; i < frame.firstEventIndex + frame.numEvents; ++i)
                {
                    auto& event = events.getReference(i);
                    auto duration = (event.finishTicks - event.startTicks) * millisecondsPerTick;
                    DBG("  " << event.name << " " << duration << " ms");
                }
            }
#endif
        }

        void reset()
        {
            for (auto& accumulator : accumulators)
            {
                accumulator.reset();
            }
            lastPaintStartTicks = 0;
            paintCount = 0;
            presentCount = 0;
            lockAcquireMaxTicks = 0;
        }

        using Ptr = ReferenceCountedObjectPtr<PaintStats>;
    };

    struct ScopedPaintEvent
    {
        ScopedPaintEvent(PaintStats::Ptr stats_, int code_, char const * const name_) :
            stats(stats_)
        {
            stats->getMostRecentFrame().addEvent(PaintEvent{ code_, name_ });
        }

        ~ScopedPaintEvent()
        {
            stats->getMostRecentFrame().getMostRecentEvent().finishTicks = Time::getHighResolutionTicks();
        }

        PaintStats::Ptr stats;
    };

    struct ScopedElapsedTime
    {
        ScopedElapsedTime(PaintStats::Ptr stats_, int accumulatorIndex_) :
            stats(stats_),
            accumulatorIndex(accumulatorIndex_)
        {
        }

        ~ScopedElapsedTime()
        {
            auto finishTicks = Time::getHighResolutionTicks();
            stats->accumulators[accumulatorIndex].addValue((finishTicks - startTicks) * stats->millisecondsPerTick);
        }

        int64 startTicks = Time::getHighResolutionTicks();
        PaintStats::Ptr stats;
        int accumulatorIndex;
    };
}

#define D2D_START_FRAME stats->startFrame(frameNumber);
#define D2D_FINISH_FRAME stats->finishFrame();
#define D2D_SCOPED_PAINT_EVENT(code, name) direct2d::ScopedPaintEvent scopedEvent(stats, direct2d::PaintEvent:: code, name);
//#define D2D_SCOPED_PAINT_EVENT(code, name) stats->getMostRecentFrame().numEvents++;

#else
    
#define D2D_START_FRAME
#define D2D_FINISH_FRAME
#define D2D_SCOPED_PAINT_EVENT(code, name)

#endif

class Direct2DLowLevelGraphicsContext   : public LowLevelGraphicsContext, public AsyncUpdater
{
public:
#if JUCE_DIRECT2D_METRICS
    Direct2DLowLevelGraphicsContext(HWND, direct2d::PaintStats::Ptr stats_);
#else
    Direct2DLowLevelGraphicsContext(HWND);
#endif
    ~Direct2DLowLevelGraphicsContext();

    //==============================================================================
    bool isVectorDevice() const override { return false; }

    void setOrigin (Point<int>) override;
    void addTransform (const AffineTransform&) override;
    float getPhysicalPixelScaleFactor() override;
    bool clipToRectangle (const Rectangle<int>&) override;
    bool clipToRectangleList (const RectangleList<int>&) override;
    void excludeClipRectangle (const Rectangle<int>&) override;
    void clipToPath (const Path&, const AffineTransform&) override;
    void clipToImageAlpha (const Image&, const AffineTransform&) override;
    bool clipRegionIntersects (const Rectangle<int>&) override;
    Rectangle<int> getClipBounds() const override;
    bool isClipEmpty() const override;

    //==============================================================================
    void saveState() override;
    void restoreState() override;
    void beginTransparencyLayer (float opacity) override;
    void endTransparencyLayer() override;

    //==============================================================================
    void setFill (const FillType&) override;
    void setOpacity (float) override;
    void setInterpolationQuality (Graphics::ResamplingQuality) override;

    //==============================================================================
    void fillRect (const Rectangle<int>&, bool replaceExistingContents) override;
    void fillRect (const Rectangle<float>&) override;
    void fillRectList (const RectangleList<float>&) override;
    bool drawRect(const Rectangle<float>&, float) override;
    void fillPath (const Path&, const AffineTransform&) override;
    bool drawPath(const Path&, const PathStrokeType& strokeType, const AffineTransform&) override;
    void drawImage (const Image& sourceImage, const AffineTransform&) override;

    //==============================================================================
    void drawLine (const Line<float>&) override;
    void setFont (const Font&) override;
    const Font& getFont() override;
    void drawGlyph (int glyphNumber, const AffineTransform&) override;
    bool supportsGlyphRun() override { return true; }
    void drawGlyphRun(Array<Glyph> const& glyphRun, const AffineTransform& transform) override;
    bool drawTextLayout(const AttributedString&, const Rectangle<float>&) override;

    void startResizing();
    void resize();
    void finishResizing();

    void addDeferredRepaint(Rectangle<int> deferredRepaint);
    bool needsRepaint();
    void startSync();
    void endSync();
    bool startAsync(int frameNumber);
    void endAsync();

    void setScaleFactor(double scale_);
    double getScaleFactor() const;

    bool drawRoundedRectangle(Rectangle<float> area, float cornerSize, float lineThickness) override;
    bool fillRoundedRectangle(Rectangle<float> area, float cornerSize) override;

    bool drawEllipse(Rectangle<float> area, float lineThickness) override;
    bool fillEllipse(Rectangle<float> area) override;

    std::function<void()> onPaintReady = nullptr;

    CriticalSection resizeLock;

    //==============================================================================
private:
    struct SavedState;

#if JUCE_DIRECT2D_METRICS
    direct2d::PaintStats::Ptr stats;
#endif

    SavedState* currentState;
    OwnedArray<SavedState> states;

    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;

    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Direct2DLowLevelGraphicsContext)
};

} // namespace juce
