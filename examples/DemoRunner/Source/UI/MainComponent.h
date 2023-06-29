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

#pragma once

#include <JuceHeader.h>
#include "DemoContentComponent.h"

//==============================================================================
class MainComponent    : public Component
{
public:
    //==============================================================================
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void paint (Graphics&) override;
    void resized() override;

    //==============================================================================
    SidePanel& getSidePanel()              { return demosPanel; }

    //==============================================================================
    void homeButtonClicked();
    void settingsButtonClicked();

    //==============================================================================
    StringArray getRenderingEngines()      { return renderingEngines; }
    int getCurrentRenderingEngine()        { return currentRenderingEngineIdx; }
    void setRenderingEngine (int index);

private:
    void parentHierarchyChanged() override;
    void updateRenderingEngine (int index);

    //==============================================================================
    std::unique_ptr<DemoContentComponent> contentComponent;
    SidePanel demosPanel  { "Demos", 250, true };

    OpenGLContext openGLContext;
    ComponentPeer* peer = nullptr;
    StringArray renderingEngines;
    int currentRenderingEngineIdx = -1;

    TextButton showDemosButton      { "Browse Demos" };

    bool isShowingHeavyweightDemo = false;
    int sidePanelWidth = 0;

#if JUCE_DIRECT2D && JUCE_DIRECT2D_METRICS
    struct StatsComponent : public Component, public ComponentListener, public Timer
    {
        StatsComponent(ComponentPeer* const peer_) :
            owner(&peer_->getComponent()),
            ownerStats(peer_->paintStats)
        {
            setOpaque(false);
            setVisible(true);
            addToDesktop(0);
            setAlwaysOnTop(true);
            setInterceptsMouseClicks(false, true);

            componentMovedOrResized(*owner, true, true);
            owner->addComponentListener(this);

            resetButton.onClick = [this]
            {
                ownerStats->reset();
            };
            addAndMakeVisible(resetButton);

            startTimer(1000);
        }

        ~StatsComponent() override
        {
            if (owner)
            {
                owner->removeComponentListener(this);
            }
        }

        void resized() override
        {
            int w = 60;
            int h = 22;
            resetButton.setBounds(getWidth() - w - 20, (getHeight() - h) / 2, w, h);
        }

        void paint(Graphics& g) override
        {
            if (!owner || !ownerStats)
            {
                return;
            }

            juce::Rectangle<float> r = getLocalBounds().removeFromBottom(25).toFloat().withX(20.0f).withWidth(getWidth() * 0.25f);
            {
                g.setColour(juce::Colours::white);
                juce::String line{ "Paint duration (ms) " };
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::paintDuration].getAverage(), 1 } << " avg. / ";
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::paintDuration].getMaxValue(), 1 } << " max / #" << ownerStats->paintCount;
                g.drawText(line, r, juce::Justification::centredLeft);
            }
            {
                r.translate(r.getWidth(), 0.0f);
                juce::String line{ "Thread paint duration (ms) " };
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::threadPaintDuration].getAverage(), 1 } << " avg. / ";
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::threadPaintDuration].getMaxValue(), 1 };
                g.drawText(line, r, juce::Justification::centredLeft);
            }
            {
                r.translate(r.getWidth(), 0.0f);
                juce::String line{ "Present (ms) " };
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::presentDuration].getAverage(), 1 } << " avg. / ";
                line << juce::String{ ownerStats->accumulators[direct2d::PaintStats::presentDuration].getMaxValue(), 1 } << " max / #" << ownerStats->presentCount;
                g.drawText(line, r, juce::Justification::centredLeft);
            }
        }

        Component::SafePointer<Component> owner;
        direct2d::PaintStats::Ptr ownerStats;

        void componentMovedOrResized(Component& component, bool /*wasMoved*/, bool /*wasResized*/) override
        {
            setBounds(component.getScreenBounds().removeFromTop(30).removeFromRight(component.proportionOfWidth(0.75f)));
        }

        void timerCallback() override
        {
            repaint();
        }

        TextButton resetButton{ "Reset" };
    };
    std::unique_ptr<StatsComponent> statsComponent;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
