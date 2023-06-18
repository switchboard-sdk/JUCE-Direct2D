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

struct PaintStats
{
    enum
    {
        paintDuration,
        paintInterval,
        present,
        numStats
    };

    StatisticsAccumulator<double> accumulators[numStats];
    int paintCount = 0;
    int presentCount = 0;
    int64 lastPaintStartTicks = 0;
    uint64_t lockAcquireMaxTicks = 0;

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
};

struct FrameTime
{
    int frameNumber = 0;
    int64_t paintStartTicks = 0;
    int64_t paintFinishTicks = 0;
    int64_t presentStartTicks = 0;
    int64_t presentFinishTicks = 0;
};

class FrameHistory
{
public:
    int const frameHistoryLength = 2048;
    std::deque<FrameTime> const& getQueue() const
    {
        return frameTimes;
    }

    void push(FrameTime& frameTime)
    {
        frameTimes.push_back(frameTime);
        while (frameTimes.size() > frameHistoryLength)
        {
            frameTimes.pop_front();
        }
    }

    void storePresentTime(int frameNumber, int64_t presentStartTicks, int64_t presentFinishTicks)
    {
        for (juce::pointer_sized_int index = frameTimes.size() - 1; index >= 0; --index)
        {
            auto& frame = frameTimes[index];
            if (frame.frameNumber == frameNumber)
            {
                frame.presentStartTicks = presentStartTicks;
                frame.presentFinishTicks = presentFinishTicks;
                break;
            }
        }
    }

private:
    std::deque<FrameTime> frameTimes;
};

class Direct2DLowLevelGraphicsContext   : public LowLevelGraphicsContext, public juce::AsyncUpdater
{
public:
    Direct2DLowLevelGraphicsContext(HWND, PaintStats& stats_, FrameHistory& frameHistory_);
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

    bool resized();

    void addDeferredRepaint(juce::Rectangle<int> deferredRepaint);
    bool needsRepaint();
    bool startPartialPaint(int frameNumber);
    bool startFullPaint();
    void end();

    void setScaleFactor(double scale_);
    double getScaleFactor() const;

    bool drawRoundedRectangle(Rectangle<float> area, float cornerSize, float lineThickness) override;
    bool fillRoundedRectangle(Rectangle<float> area, float cornerSize) override;

    bool drawEllipse(Rectangle<float> area, float lineThickness) override;
    bool fillEllipse(Rectangle<float> area) override;

    std::function<void()> onPaintReady = nullptr;

    //==============================================================================
private:
    struct SavedState;

    PaintStats& stats;

    SavedState* currentState;
    OwnedArray<SavedState> states;

    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;

    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Direct2DLowLevelGraphicsContext)
};

} // namespace juce
