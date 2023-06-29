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

#if !defined(_WINDEF_) && !defined(__INTELLISENSE__)
class HWND__; // Forward or never
typedef HWND__* HWND;
#endif

#if JUCE_DIRECT2D_METRICS

namespace direct2d
{
    struct PaintStats : public ReferenceCountedObject
    {
        enum
        {
            paintDuration,
            threadPaintDuration,
            presentDuration,
            numStats
        };

        StatisticsAccumulator<double> accumulators[numStats];
        int64 const creationTime = Time::getMillisecondCounter();
        double const millisecondsPerTick = 1000.0 / (double)Time::getHighResolutionTicksPerSecond();
        int paintCount = 0;
        int presentCount = 0;
        int64 lastPaintStartTicks = 0;
        uint64 lockAcquireMaxTicks = 0;

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
    bool startAsync();
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
