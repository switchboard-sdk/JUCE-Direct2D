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

/*

    Presentation presentations[2];
    int presentationIndex = 0;
    std::atomic<Presentation*> paintedPresentation = nullptr;

    while (thread running)
    {
        wait(-1);

        Presentation* threadPresentation = paintedPresentation.exchange(nullptr);
        if (!threadPresentation)
        {
            continue;
        }

        jassert(threadPresentation->isPainted())

        {
            present();  // full window
            asyncUpdate();
        }
    }

    asyncUpdate()
    {
        threadPresentation->state = clear

        jassert(presentations + presentationIndex != threadPresentation)

        d2dPaintSync()
    }

    d2dPaintSync()
    {
        if (!presentations[presentationIndex ^ 1].isClear())
        {
            return;
        }

        Presentation* paintPresentation = presentations + presentationIndex

        jassert(paintPresentation->isClear())

        if (paintPresentation->update area is empty)
        {
            return;
        }

        paintPresentation->state = painting

        paint

        paintPresentation->state = painted
        paintedPresentation = paintPresentation
        presentationIndex ^= 1

        notify thread
    }

    addDeferredRepaint(area)
    {
        Presentation* presentation =  presentations[presentationIndex];
        presentation->add(area)

        jassert(presentation->isClear())
    }


    Peer
    ----
    WM_PAINT
        get update region from window
        addDeferredRepaint(region)
        validate update region

        d2dPaintSync()

    repaint
        addDeferredRepaint(area)

    performAnyPendingRepaintsNow
        d2dPaintSync()


*/

namespace juce
{

    namespace direct2d
    {
        template <typename Type>
        D2D1_RECT_F rectangleToRectF(const Rectangle<Type>& r)
        {
            return { (float)r.getX(), (float)r.getY(), (float)r.getRight(), (float)r.getBottom() };
        }

        template <typename Type>
        RECT rectangleToRECT(const Rectangle<Type>& r)
        {
            return { r.getX(), r.getY(), r.getRight(), r.getBottom() };
        }

        template <typename Type>
        Rectangle<int> RECTToRectangle(RECT const& r)
        {
            return Rectangle<int>::leftTopRightBottom(r.left, r.top, r.right, r.bottom);
        }

        static D2D1_COLOR_F colourToD2D(Colour c)
        {
            return { c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), c.getFloatAlpha() };
        }

        static bool isTransformOnlyTranslationOrScale(AffineTransform const& transform)
        {
            return approximatelyEqual(transform.mat01, 0.0f) && approximatelyEqual(transform.mat10, 0.0f);
        }

        static void pathToGeometrySink(const Path& path, ID2D1GeometrySink* sink, const AffineTransform& transform)
        {
            //
            // Every call to BeginFigure must have a matching call to EndFigure. But - the Path does not necessarily
            // have matching startNewSubPath and closePath markers. The figureStarted flag indicates if an extra call
            // to BeginFigure or EndFigure is needed during the iteration loop or when exiting this function.
            //
            Path::Iterator it(path);
            bool figureStarted = false;

            while (it.next())
            {
                switch (it.elementType)
                {
                case Path::Iterator::cubicTo:
                {
                    jassert(figureStarted);

                    transform.transformPoint(it.x1, it.y1);
                    transform.transformPoint(it.x2, it.y2);
                    transform.transformPoint(it.x3, it.y3);

                    sink->AddBezier({ { it.x1, it.y1 }, { it.x2, it.y2 }, { it.x3, it.y3 } });
                    break;
                }

                case Path::Iterator::lineTo:
                {
                    jassert(figureStarted);

                    transform.transformPoint(it.x1, it.y1);
                    sink->AddLine({ it.x1, it.y1 });
                    break;
                }

                case Path::Iterator::quadraticTo:
                {
                    jassert(figureStarted);

                    transform.transformPoint(it.x1, it.y1);
                    transform.transformPoint(it.x2, it.y2);
                    sink->AddQuadraticBezier({ { it.x1, it.y1 }, { it.x2, it.y2 } });
                    break;
                }

                case Path::Iterator::closePath:
                {
                    if (figureStarted)
                    {
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        figureStarted = false;
                    }
                    break;
                }

                case Path::Iterator::startNewSubPath:
                {
                    if (figureStarted)
                    {
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    }

                    transform.transformPoint(it.x1, it.y1);
                    sink->BeginFigure({ it.x1, it.y1 }, D2D1_FIGURE_BEGIN_FILLED);
                    figureStarted = true;
                    break;
                }
                }
            }

            if (figureStarted)
            {
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
        }

        static D2D1::Matrix3x2F transformToMatrix(const AffineTransform& transform)
        {
            return { transform.mat00, transform.mat10, transform.mat01, transform.mat11, transform.mat02, transform.mat12 };
        }

        static D2D1_POINT_2F pointTransformed(int x, int y, const AffineTransform& transform)
        {
            transform.transformPoint(x, y);
            return { (FLOAT)x, (FLOAT)y };
        }

        static void rectToGeometrySink(const Rectangle<int>& rect, ID2D1GeometrySink* sink, const AffineTransform& transform)
        {
            sink->BeginFigure(pointTransformed(rect.getX(), rect.getY(), transform), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(pointTransformed(rect.getRight(), rect.getY(), transform));
            sink->AddLine(pointTransformed(rect.getRight(), rect.getBottom(), transform));
            sink->AddLine(pointTransformed(rect.getX(), rect.getBottom(), transform));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }

        bool isTearingSupported()
        {
            // Rather than create the 1.5 factory interface directly, we create the 1.4
            // interface and query for the 1.5 interface. This will enable the graphics
            // debugging tools which might not support the 1.5 factory interface.
            ComSmartPtr<IDXGIFactory4> factory4;
            auto hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)factory4.resetAndGetPointerAddress());
            if (SUCCEEDED(hr))
            {
                auto factory5 = factory4.getInterface<IDXGIFactory5>();
                if (factory5)
                {
                    BOOL allowTearing;
                    hr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
                    return SUCCEEDED(hr) && allowTearing;
                }
            }

            return false;
        }

        class UpdateRegion
        {
        public:
            ~UpdateRegion()
            {
                clear();
            }

            void refresh(HWND windowHandle)
            {
                numRect = 0;

                regionHandle = CreateRectRgn(0, 0, 0, 0);
                auto regionType = GetUpdateRgn(windowHandle, regionHandle, false);
                if (regionType == SIMPLEREGION || regionType == COMPLEXREGION)
                {
                    auto regionDataBytes = GetRegionData(regionHandle, (DWORD)block.getSize(), (RGNDATA*)block.getData());
                    if (regionDataBytes > block.getSize())
                    {
                        block.ensureSize(regionDataBytes);
                        regionDataBytes = GetRegionData(regionHandle, (DWORD)block.getSize(), (RGNDATA*)block.getData());
                    }

                    if (regionDataBytes > 0)
                    {
                        auto header = (RGNDATAHEADER const* const)block.getData();
                        if (header->iType == RDH_RECTANGLES)
                        {
                            numRect = header->nCount;
                        }
                    }
                }
            }

            void clear()
            {
                numRect = 0;
                DeleteObject(regionHandle);
                regionHandle = nullptr;
            }

            uint32 getNumRECT() const
            {
                return numRect;
            }

            RECT* getRECTArray()
            {
                auto header = (RGNDATAHEADER const* const)block.getData();
                return (RECT*)(header + 1);
            }

            void addToRectangleList(RectangleList<int>& rectangleList)
            {
                rectangleList.ensureStorageAllocated(rectangleList.getNumRectangles() + getNumRECT());
                for (uint32 i = 0; i < getNumRECT(); ++i)
                {
                    rectangleList.add(RECTToRectangle<int>(getRECTArray()[i]));
                }
            }

            HRGN regionHandle = nullptr;

        private:
            MemoryBlock block{ 1024 };
            uint32 numRect = 0;
        };

    } // namespace Direct2D

//==============================================================================

struct Direct2DLowLevelGraphicsContext::Pimpl : public Thread
{
    Pimpl(Direct2DLowLevelGraphicsContext& owner_, HWND hwnd_, bool tearingSupported_, FrameHistory& frameHistory_) :
        Thread("Direct2DLowLevelGraphicsContext"),
        owner(owner_),
        hwnd(hwnd_),
        frameHistory(frameHistory_),
        swapEffect(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL),
        bufferCount(2),
        dxgiScaling(DXGI_SCALING_STRETCH),
        swapChainFlags(tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
        presentSyncInterval(tearingSupported_ ? 0 : 1),
        presentFlags(tearingSupported_ ? DXGI_PRESENT_ALLOW_TEARING : 0)
    {
#if JUCE_DEBUG
        D2D1_FACTORY_OPTIONS options{ D2D1_DEBUG_LEVEL_INFORMATION };
#else
        D2D1_FACTORY_OPTIONS options{ D2D1_DEBUG_LEVEL_NONE };
#endif

        auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(d2dDedicatedFactory.resetAndGetPointerAddress()));
        jassertquiet(SUCCEEDED(hr));

        startThread(Priority::highest);
    }

    ~Pimpl() override
    {
        stopThread(1000);
    }

    //
    // ScopedGeometryWithSink creates an ID2D1PathGeometry object with an open sink. 
    // D.R.Y. for rectToPathGeometry, rectListToPathGeometry, and pathToPathGeometry
    //
    struct ScopedGeometryWithSink
    {
        ScopedGeometryWithSink(ID2D1Factory* factory, D2D1_FILL_MODE fillMode)
        {
            auto hr = factory->CreatePathGeometry(geometry.resetAndGetPointerAddress());
            if (SUCCEEDED(hr))
            {
                hr = geometry->Open(sink.resetAndGetPointerAddress());
                if (SUCCEEDED(hr))
                {
                    sink->SetFillMode(fillMode);
                }
            }
        }

        ~ScopedGeometryWithSink()
        {
            if (sink != nullptr)
            {
                auto hr = sink->Close();
                jassertquiet(SUCCEEDED(hr));
            }
        }

        ComSmartPtr<ID2D1PathGeometry> geometry;
        ComSmartPtr<ID2D1GeometrySink> sink;
    };

    ComSmartPtr<ID2D1Geometry> rectToPathGeometry(const Rectangle<int>& rect, const AffineTransform& transform, D2D1_FILL_MODE fillMode)
    {
        ScopedGeometryWithSink objects{ d2dDedicatedFactory, fillMode };

        if (objects.sink != nullptr)
        {
            direct2d::rectToGeometrySink(rect, objects.sink, transform);
            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ComSmartPtr<ID2D1Geometry> rectListToPathGeometry (const RectangleList<int>& clipRegion, const AffineTransform& transform, D2D1_FILL_MODE fillMode)
    {
        ScopedGeometryWithSink objects{ d2dDedicatedFactory, fillMode };

        if (objects.sink != nullptr)
        {
            for (int i = clipRegion.getNumRectangles(); --i >= 0;)
                direct2d::rectToGeometrySink(clipRegion.getRectangle(i), objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ComSmartPtr<ID2D1Geometry> pathToPathGeometry (const Path& path, const AffineTransform& transform)
    {
        ScopedGeometryWithSink objects{ d2dDedicatedFactory, path.isUsingNonZeroWinding() ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE };

        if (objects.sink != nullptr)
        {
            direct2d::pathToGeometrySink(path, objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ID2D1DeviceContext* const getDeviceContext() const 
    {
        return deviceContext;
    }

    ID2D1SolidColorBrush* const getColourBrush() const
    {
        return colourBrush;
    }

    Rectangle<int> getClientRect() const
    {
        RECT windowRect;
        GetClientRect(hwnd, &windowRect);

        return juce::Rectangle<int>::leftTopRightBottom(windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
    }

    bool resized()
    {
        //
        // Get the width & height from the client area; make sure width and height are between 1 and 16384
        //
        int constexpr minSize = 1;
        int constexpr maxSize = 16384;
        auto windowRect = getClientRect().getUnion({ minSize, minSize }).getIntersection({ maxSize, maxSize });
        if (bufferBounds == windowRect)
        {
            return false;
        }

        bufferBounds = windowRect;

        //
        // Resize the swap chain 
        //
        if (deviceContext != nullptr)
        {
            deviceContext->SetTarget(nullptr); // xxx this may be redundant
        }

        if (swapChain != nullptr)
        {
            swapChainBuffer = nullptr; // must release swap chain buffer before calling ResizeBuffers

            auto scaledWidth = roundToInt(bufferBounds.getWidth() * dpiScalingFactor);
            auto scaledHeight = roundToInt(bufferBounds.getHeight() * dpiScalingFactor);
            auto hr = swapChain->ResizeBuffers(0, scaledWidth, scaledHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags);

            if (SUCCEEDED(hr))
            {
                createSwapChainBuffer();

                return true;
            }
            else
            {
                releaseDeviceContext();
            }
        }

        return false;
    }

    void addDeferredRepaint(juce::Rectangle<int> deferredRepaint)
    {
        auto* const presentation = presentations + presentationIndex;
        presentation->paintAreas.add(deferredRepaint);
    }

    bool needsRepaint() const
    {
        auto* const presentation = presentations + presentationIndex;
        return presentation->paintAreas.getNumRectangles() > 0;
    }

    bool isReadyToPaint() const
    {
        return presentations[presentationIndex ^ 1].state == Presentation::clear;
    }

    bool startRender(int frameNumber, juce::Rectangle<int>& initialClipBounds)
    {
        //
        // Ready to paint? Return if the previous presentation has not been presented
        //
        if (!isReadyToPaint())
        {
            return false;
        }

        auto* const presentation = presentations + presentationIndex;

        //
        // Any areas to update?
        //
        updateRegion.refresh(hwnd);
        if (updateRegion.getNumRECT() == 0 && presentation->paintAreas.getNumRectangles() == 0)
        {
            return false;
        }
        updateRegion.addToRectangleList(presentation->paintAreas);
        ValidateRgn(hwnd, updateRegion.regionHandle);

        initialClipBounds = presentation->paintAreas.getBounds();

        presentation->presentEntireWindow = initialClipBounds.contains(bufferBounds);

        //
        // Start painting
        //
        presentation->frameNumber = frameNumber;
        presentation->state = Presentation::painting;

#if 0 // JUCE_DEBUG
        for (auto const area : presentation->paintAreas)
        {
            DBG("   area " << area.toString());
        }

        for (uint32 i = 0; i < updateRegion.getNumRECT(); ++i)
        {
            DBG("   update " << direct2d::RECTToRectangle<int>(updateRegion.getRECTArray()[i]).toString());
        }
#endif

        createDeviceContext();
        if (deviceContext != nullptr)
        {
            createSwapChainBuffer();
            if (swapChainBuffer != nullptr)
            {
                deviceContext->SetTarget(swapChainBuffer);
                deviceContext->BeginDraw();
            }
        }

        return true;
    }

    void finishRender()
    {
        if (deviceContext != nullptr && swapChain != nullptr)
        {
            auto hr = deviceContext->EndDraw();
            deviceContext->SetTarget(nullptr);
            if (FAILED(hr))
            {
                return;
            }

            auto* const presentation = presentations + presentationIndex;
            auto paintBounds = presentation->paintAreas.getBounds();
            if (!bufferBounds.intersects(paintBounds) || paintBounds.isEmpty())
            {
                return;
            }

            presentation->state = Presentation::painted;

            {
                presentation->dirtyRectangles.ensureStorageAllocated(presentation->paintAreas.getNumRectangles());
                presentation->dirtyRectangles.clearQuick();

                for (auto const& paintArea : presentation->paintAreas)
                {
                    presentation->dirtyRectangles.add(direct2d::rectangleToRECT(paintArea));
                }
            }

            paintedPresentation.store(presentation);
            presentationIndex ^= 1;
            notify();

            if (S_OK != hr && DXGI_STATUS_OCCLUDED != hr)
            {
                releaseDeviceContext();
            }

#if 0
            if (SUCCEEDED(hr))
            {
                auto deferredRepaintsBounds = deferredRepaints.getBounds();
                if (swapChainPartialPresentReady && bufferBounds.intersects(deferredRepaintsBounds) && !deferredRepaintsBounds.isEmpty())
                {
                    dirtyRectangles.ensureStorageAllocated(deferredRepaints.getNumRectangles());
                    dirtyRectangles.clearQuick();

                    for (auto const& deferredRepaint : deferredRepaints)
                    {
                        dirtyRectangles.add(direct2d::rectangleToRECT(deferredRepaint));
                    }

                    DXGI_PRESENT_PARAMETERS presentParameters
                    {
                        (uint32)dirtyRectangles.size(),
                        dirtyRectangles.getRawDataPointer(),
                        nullptr,
                        nullptr
                    };

                    jassert(swapChainPartialPresentReady);

#if 0  // JUCE_DEBUG
                    DBG("   #dirty " << (int)dirtyRectangles.size());
                    for (auto dirtyRectangle : dirtyRectangles)
                    {
                        DBG("   " << (int)dirtyRectangle.left << ", " << (int)dirtyRectangle.top << ", " << (int)dirtyRectangle.right << ", " << (int)dirtyRectangle.bottom);
                    }
#endif

                    hr = swapChain->Present1(presentSyncInterval, presentFlags, &presentParameters);
                    jassert(SUCCEEDED(hr));

                    ???
                    //ValidateRgn(hwnd, updateRegion.regionHandle);

                    stats.presentCount++;
                }
                else
                {
                    hr = swapChain->Present(presentSyncInterval, presentFlags);
                    //ValidateRect(hwnd, nullptr);
                
                    swapChainPartialPresentReady = true;
                    stats.presentCount++;
                }

            }

            if (S_OK != hr && DXGI_STATUS_OCCLUDED != hr)
            {
                releaseDeviceContext();
            }
#endif
        }

        //DBG("finishRender\n");
    }

    void run() override
    {
        bool fullPresentDone = false;

        while (!threadShouldExit())
        {
            //
            // Wait until a presentation is ready
            //
            wait(-1);

            //
            // Is a presentation ready?
            //
            auto presentation = paintedPresentation.exchange(nullptr);
            if (presentation == nullptr)
            {
                continue;
            }

            jassert(presentation->state == Presentation::painted);
            
            //
            // If this swap chain buffer has never been painted, present the entire window
            // 
            // Otherwise, present the update region
            //
            {
                DXGI_PRESENT_PARAMETERS presentParameters{};
#if JUCE_DIRECT2D_METRICS
                presentation->presentStartTicks = Time::getHighResolutionTicks();
#endif

                if (fullPresentDone && !presentation->presentEntireWindow)
                {
                    presentParameters.DirtyRectsCount = (uint32)presentation->dirtyRectangles.size();
                    presentParameters.pDirtyRects = presentation->dirtyRectangles.getRawDataPointer();
                }

                presentation->status = swapChain->Present1(presentSyncInterval, presentFlags, &presentParameters);
                jassert(SUCCEEDED(presentation->status));

                fullPresentDone = SUCCEEDED(presentation->status);

#if JUCE_DIRECT2D_METRICS
                presentation->presentFinishTicks = Time::getHighResolutionTicks();
                owner.stats.presentCount++;
#endif
            }

            //
            // Post a message indicating that this presentation is done; ready for the next one
            //
            {
                struct PresentDoneMessage : public CallbackMessage
                {
                    PresentDoneMessage(Pimpl* that_, Presentation* presentation_) : 
                        that(that_),
                        presentation(presentation_) 
                    {
                    }
                    ~PresentDoneMessage() override = default;

                    void messageCallback() override
                    {
                        //
                        // Back on the message thread
                        //
                        if (that)
                        {
#if JUCE_DIRECT2D_METRICS
                            that->frameHistory.storePresentTime(presentation->frameNumber, presentation->presentStartTicks, presentation->presentFinishTicks);
#endif
                                
                            if (presentation)
                            {
                                //
                                // Release the device context if Present1 returned an error
                                //
                                auto status = presentation->status;
                                if (status != S_OK && status != DXGI_STATUS_OCCLUDED)
                                {
                                    that->releaseDeviceContext();
                                }

                                presentation->reset();
                            }

                            if (that->owner.onPaintReady)
                            {
                                that->owner.onPaintReady();
                            }
                        }
                    }

                    WeakReference<Pimpl> that;
                    Presentation* presentation;
                };

                (new PresentDoneMessage{ this, presentation })->post();
            }
        }
    }

    void setScaleFactor(double scale_)
    {
        dpiScalingFactor = scale_;

        updateDeviceContextDPI();
        resized();
    }

    double getScaleFactor() const
    {
        return dpiScalingFactor;
    }

    HWND hwnd = nullptr;
    SharedResourcePointer<Direct2DFactories> sharedFactories;
    ComSmartPtr<ID2D1Factory1> d2dDedicatedFactory;
    ComSmartPtr<ID2D1StrokeStyle> strokeStyle;

    direct2d::UpdateRegion updateRegion;

private:
    Direct2DLowLevelGraphicsContext& owner;
    FrameHistory& frameHistory;
    DXGI_SWAP_EFFECT const swapEffect;
    UINT const bufferCount;
    DXGI_SCALING const dxgiScaling;
    double dpiScalingFactor = 1.0;
    juce::Rectangle<int> bufferBounds{ 1, 1 };
    uint32 const swapChainFlags;
    uint32 const presentSyncInterval;
    uint32 const presentFlags;
    ComSmartPtr<ID2D1DeviceContext> deviceContext;
    ComSmartPtr<IDXGISwapChain1> swapChain;
    ComSmartPtr<ID2D1Bitmap1> swapChainBuffer;
    Array<RECT> dirtyRectangles;
    ComSmartPtr<ID2D1SolidColorBrush> colourBrush;
    ComSmartPtr<IDCompositionDevice> compositionDevice;
    ComSmartPtr<IDCompositionTarget> compositionTarget;
    ComSmartPtr<IDCompositionVisual> compositionVisual;

    struct Presentation
    {
        int frameNumber = -1;
        HRESULT status = S_OK;
        bool presentEntireWindow = true;
        juce::RectangleList<int> paintAreas;
        Array<RECT> dirtyRectangles;
        enum State
        {
            clear,
            painting,
            painted
        } state = clear;
        juce::RectangleList<int> paintAreas;
        //ComSmartPtr<ID2D1CommandList> commandList;

        void reset()
        {
            state = clear;
            paintAreas.clear();
            dirtyRectangles.clearQuick();
        }

        int64_t presentStartTicks = 0;
        int64_t presentFinishTicks = 0;
    } presentations[2];
    int presentationIndex = 0;
    std::atomic<Presentation*> paintedPresentation = nullptr;

    void createDeviceContext()
    {
        if (d2dDedicatedFactory != nullptr)
        {
            if (deviceContext == nullptr)
            {
                // This flag adds support for surfaces with a different color channel ordering
                // than the API default. It is required for compatibility with Direct2D.
                UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if JUCE_DEBUG
                creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

                ComSmartPtr<ID3D11Device> direct3DDevice;
                auto hr = D3D11CreateDevice(nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    creationFlags,
                    nullptr, 0,
                    D3D11_SDK_VERSION,
                    direct3DDevice.resetAndGetPointerAddress(),
                    nullptr,
                    nullptr);
                if (SUCCEEDED(hr))
                {
                    ComSmartPtr<IDXGIDevice> dxgiDevice;
                    hr = direct3DDevice->QueryInterface(dxgiDevice.resetAndGetPointerAddress());
                    if (SUCCEEDED(hr))
                    {
                        ComSmartPtr<IDXGIAdapter> dxgiAdapter;
                        hr = dxgiDevice->GetAdapter(dxgiAdapter.resetAndGetPointerAddress());
                        if (SUCCEEDED(hr))
                        {
                            ComSmartPtr<IDXGIFactory2> dxgiFactory;
                            hr = dxgiAdapter->GetParent(__uuidof(dxgiFactory), reinterpret_cast<void**>(dxgiFactory.resetAndGetPointerAddress()));
                            if (SUCCEEDED(hr))
                            {
                                DXGI_SWAP_CHAIN_DESC1 swapChainDescription = {};
                                swapChainDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                swapChainDescription.Width = bufferBounds.getWidth();
                                swapChainDescription.Height = bufferBounds.getHeight();
                                swapChainDescription.SampleDesc.Count = 1;
                                swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                swapChainDescription.BufferCount = bufferCount;
                                swapChainDescription.SwapEffect = swapEffect;
                                swapChainDescription.Scaling = dxgiScaling;
                                swapChainDescription.Flags = swapChainFlags;

                                hr = dxgiFactory->CreateSwapChainForComposition(direct3DDevice,
                                    &swapChainDescription,
                                    nullptr,
                                    swapChain.resetAndGetPointerAddress());

                                if (SUCCEEDED(hr))
                                {
                                    ComSmartPtr<ID2D1Device> direct2DDevice;
                                    hr = d2dDedicatedFactory->CreateDevice(dxgiDevice, direct2DDevice.resetAndGetPointerAddress());
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = direct2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, deviceContext.resetAndGetPointerAddress());
                                        if (SUCCEEDED(hr))
                                        {
                                            updateDeviceContextDPI();
                                        }
                                    }
                                }

                                if (SUCCEEDED(hr))
                                {
                                    hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(compositionDevice.resetAndGetPointerAddress()));
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = compositionDevice->CreateTargetForHwnd(hwnd, FALSE, compositionTarget.resetAndGetPointerAddress());
                                        if (SUCCEEDED(hr))
                                        {
                                            hr = compositionDevice->CreateVisual(compositionVisual.resetAndGetPointerAddress());
                                            if (SUCCEEDED(hr))
                                            {
                                                hr = compositionTarget->SetRoot(compositionVisual);
                                                if (SUCCEEDED(hr))
                                                {
                                                    hr = compositionVisual->SetContent(swapChain);
                                                    if (SUCCEEDED(hr))
                                                    {
                                                        hr = compositionDevice->Commit();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                jassert(SUCCEEDED(hr));
            }

            if (colourBrush == nullptr && deviceContext != nullptr)
            {
                auto hr = deviceContext->CreateSolidColorBrush(D2D1::ColorF::ColorF(0.0f, 0.0f, 0.0f, 1.0f), colourBrush.resetAndGetPointerAddress());
                jassertquiet(SUCCEEDED(hr));
            }
        }
    }

    void releaseDeviceContext()
    {
        colourBrush = nullptr;
        swapChainBuffer = nullptr;
        swapChain = nullptr;
        deviceContext = nullptr;
        for (auto& presentation : presentations)
        {
            presentation.reset();
        }
    }

    void createSwapChainBuffer()
    {
        if (deviceContext != nullptr && swapChain != nullptr && swapChainBuffer == nullptr)
        {
            ComSmartPtr<IDXGISurface> surface;
            auto hr = swapChain->GetBuffer(0, __uuidof(surface), reinterpret_cast<void**>(surface.resetAndGetPointerAddress()));
            if (SUCCEEDED(hr))
            {
                D2D1_BITMAP_PROPERTIES1 bitmapProperties = {};
                bitmapProperties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                bitmapProperties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                bitmapProperties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
                hr = deviceContext->CreateBitmapFromDxgiSurface(surface, bitmapProperties, swapChainBuffer.resetAndGetPointerAddress());
                jassert(SUCCEEDED(hr));
            }
        }
    }

    void updateDeviceContextDPI()
    {
        if (deviceContext)
        {
            float windowsDefaultDPI = 96.0f;
            float scaledDPI = windowsDefaultDPI * (float)dpiScalingFactor;
            deviceContext->SetDpi(scaledDPI, scaledDPI);
        }
    }

    JUCE_DECLARE_WEAK_REFERENCEABLE(Pimpl)
};

//==============================================================================
struct Direct2DLowLevelGraphicsContext::SavedState
{
public:
    SavedState (Direct2DLowLevelGraphicsContext& owner_)
        : owner (owner_)
    {
        if (owner.currentState != nullptr)
        {
            // xxx seems like a very slow way to create one of these, and this is a performance
            // bottleneck.. Can the same internal objects be shared by multiple state objects, maybe using copy-on-write?
            setFill (owner.currentState->fillType);
            currentBrush = owner.currentState->currentBrush;
            clipRegion = owner.currentState->clipRegion;
            currentTransform = owner.currentState->currentTransform;

            font = owner.currentState->font;
            currentFontFace = owner.currentState->currentFontFace;

            interpolationMode = owner.currentState->interpolationMode;
        }
        else
        {
            if (auto deviceContext = owner.pimpl->getDeviceContext())
            {
                const auto size = deviceContext->GetPixelSize();
                clipRegion.setSize(size.width, size.height);
            }
            setFill(FillType(Colours::black));
        }
    }

    ~SavedState()
    {
        popLayers();
        clearFont();
        clearFill();
    }

    void pushLayer(const D2D1_LAYER_PARAMETERS& layerParameters)
    {
        if (auto deviceContext = owner.pimpl->getDeviceContext())
        {
	        //
	        // Clipping and transparency are all handled by pushing Direct2D layers. The SavedState creates an internal stack
	        // of Layer objects to keep track of how many layers need to be popped.
	        // 
	        // Pass nullptr for the layer to allow Direct2D to manage the layers (Windows 8 or later)
	        //
	        deviceContext->PushLayer(layerParameters, nullptr);
	
	        pushedLayers.add(new Layer{ deviceContext });
        }
    }

    void pushGeometryClipLayer(ComSmartPtr<ID2D1Geometry> geometry)
    {
        if (geometry != nullptr)
        {
            pushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry));
        }
    }

    void pushAxisAlignedClipLayer(Rectangle<int> r)
    {
        if (auto deviceContext = owner.pimpl->getDeviceContext())
        {
            deviceContext->PushAxisAlignedClip(direct2d::rectangleToRectF(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            pushedLayers.add(new AxisAlignedClipLayer{ deviceContext });
        }
    }

    void popLayers()
    {
        //
        // Pop each layer in reverse order
        //
        for (int index = pushedLayers.size() - 1; index >= 0; --index)
        {
            pushedLayers[index]->pop();
        }

        pushedLayers.clear(true /* deleteObjects */);
    }

    void setFill (const FillType& newFillType)
    {
        if (fillType != newFillType)
        {
            fillType = newFillType;
            clearFill();
        }
    }

    void clearFont()
    {
        currentFontFace = localFontFace = nullptr;
    }

    void setFont (const Font& newFont)
    {
        if (font != newFont)
        {
            font = newFont;
            clearFont();
        }
    }

    void createFont()
    {
        if (currentFontFace == nullptr)
        {
            auto typeface = font.getTypefacePtr();
            auto directWriteTypeface = dynamic_cast<WindowsDirectWriteTypeface*> (typeface.get());
            if (directWriteTypeface)
            {
                currentFontFace = directWriteTypeface->getIDWriteFontFace();
                fontHeightToEmSizeFactor = directWriteTypeface->getUnitsToHeightScaleFactor();
            }
        }
    }

    void setOpacity (float newOpacity)
    {
        fillType.setOpacity (newOpacity);
        if (fillType.isColour())
        {
            updateColourBrush();
        }
    }

    void clearFill()
    {
        gradientStops = nullptr;
        linearGradient = nullptr;
        radialGradient = nullptr;
        bitmapBrush = nullptr;
        currentBrush = nullptr;
    }

    void createBrush()
    {
        auto deviceContext = owner.pimpl->getDeviceContext();
        if (currentBrush == nullptr && deviceContext != nullptr)
        {
            if (fillType.isColour())
            {
                updateColourBrush();

                currentBrush = owner.pimpl->getColourBrush();
            }
            else if (fillType.isTiledImage())
            {
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), direct2d::transformToMatrix (fillType.transform) };
                auto bmProps = D2D1::BitmapBrushProperties (D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP);

                auto image = fillType.image;

                D2D1_SIZE_U size = { (UINT32) image.getWidth(), (UINT32) image.getHeight() };
                auto bp = D2D1::BitmapProperties();

                image = image.convertedToFormat (Image::ARGB);
                Image::BitmapData bd (image, Image::BitmapData::readOnly);
                bp.pixelFormat = deviceContext->GetPixelFormat();
                bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

                ComSmartPtr<ID2D1Bitmap> tiledImageBitmap;
                auto hr = deviceContext->CreateBitmap (size, bd.data, bd.lineStride, bp, tiledImageBitmap.resetAndGetPointerAddress());
                jassert(SUCCEEDED(hr));
                if (SUCCEEDED(hr))
                {
                    hr = deviceContext->CreateBitmapBrush(tiledImageBitmap, bmProps, brushProps, bitmapBrush.resetAndGetPointerAddress());
                    jassert(SUCCEEDED(hr));
                    if (SUCCEEDED(hr))
                    {
                        currentBrush = bitmapBrush;
                    }
                }
            }
            else if (fillType.isGradient())
            {
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), direct2d::transformToMatrix(fillType.transform) };
                const int numColors = fillType.gradient->getNumColours();

                HeapBlock<D2D1_GRADIENT_STOP> stops (numColors);

                for (int i = fillType.gradient->getNumColours(); --i >= 0;)
                {
                    stops[i].color = direct2d::colourToD2D (fillType.gradient->getColour (i));
                    stops[i].position = (FLOAT) fillType.gradient->getColourPosition (i);
                }

                deviceContext->CreateGradientStopCollection (stops.getData(), numColors, gradientStops.resetAndGetPointerAddress());

                if (fillType.gradient->isRadial)
                {
                    const auto p1 = fillType.gradient->point1;
                    const auto p2 = fillType.gradient->point2;
                    const auto r = p1.getDistanceFrom(p2);
                    const auto props = D2D1::RadialGradientBrushProperties ({ p1.x, p1.y }, {}, r, r);

                    deviceContext->CreateRadialGradientBrush (props, brushProps, gradientStops, radialGradient.resetAndGetPointerAddress());
                    currentBrush = radialGradient;
                }
                else
                {
                    const auto p1 = fillType.gradient->point1;
                    const auto p2 = fillType.gradient->point2;
                    const auto props = D2D1::LinearGradientBrushProperties ({ p1.x, p1.y }, { p2.x, p2.y });

                    deviceContext->CreateLinearGradientBrush (props, brushProps, gradientStops, linearGradient.resetAndGetPointerAddress());

                    currentBrush = linearGradient;
                }
            }
        }
    }

    void beginTransparency(float opacity)
    {
        pushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),
            nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(),
            opacity));
    }

    void updateColourBrush()
    {
        if (auto colourBrush = owner.pimpl->getColourBrush())
        {
            auto colour = direct2d::colourToD2D(fillType.colour);
            colourBrush->SetColor(colour);

            colourBrush->SetOpacity(fillType.getOpacity());
        }
    }

    //
    // Layer struct to keep track of pushed Direct2D layers.
    // 
    // Most layers need to be popped by calling PopLayer, unless it's an axis aligned clip layer
    //
    struct Layer
    {
        Layer(ID2D1DeviceContext* deviceContext_) :
            deviceContext(deviceContext_)
        {
            jassert(deviceContext_ != nullptr);
        }
        virtual ~Layer() = default;

        virtual void pop()
        {
            deviceContext->PopLayer();
        }

        ID2D1DeviceContext* deviceContext;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Layer)
    };

    struct AxisAlignedClipLayer : public Layer
    {
        AxisAlignedClipLayer(ID2D1DeviceContext* deviceContext_) :
            Layer(deviceContext_)
        {
        }
        ~AxisAlignedClipLayer() override = default;

        void pop() override
        {
            deviceContext->PopAxisAlignedClip();
        }
    };

    Direct2DLowLevelGraphicsContext& owner;

    RenderingHelpers::TranslationOrTransform currentTransform;
    Rectangle<int> clipRegion;

    Font font;
    float fontHeightToEmSizeFactor = 1.0f;

    IDWriteFontFace* currentFontFace = nullptr;
    ComSmartPtr<IDWriteFontFace> localFontFace;

    OwnedArray<Layer> pushedLayers;

    ID2D1Brush* currentBrush = nullptr;
    ComSmartPtr<ID2D1BitmapBrush> bitmapBrush;
    ComSmartPtr<ID2D1LinearGradientBrush> linearGradient;
    ComSmartPtr<ID2D1RadialGradientBrush> radialGradient;
    ComSmartPtr<ID2D1GradientStopCollection> gradientStops;

    FillType fillType;

    D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SavedState)
};

//==============================================================================
Direct2DLowLevelGraphicsContext::Direct2DLowLevelGraphicsContext (HWND hwnd_, PaintStats& stats_, FrameHistory& frameHistory_)
    : currentState (nullptr),
      pimpl (new Pimpl(*this, hwnd_, direct2d::isTearingSupported(), frameHistory_)),
      stats(stats_)
{
    resized();
}

Direct2DLowLevelGraphicsContext::~Direct2DLowLevelGraphicsContext()
{
    states.clear();
}

bool Direct2DLowLevelGraphicsContext::resized()
{
    return pimpl->resized();
}

void Direct2DLowLevelGraphicsContext::addDeferredRepaint(juce::Rectangle<int> deferredRepaint)
{
    //DBG("  ----addDeferredRepaint " << deferredRepaint.toString());
    pimpl->addDeferredRepaint(deferredRepaint);

    triggerAsyncUpdate();
}

bool Direct2DLowLevelGraphicsContext::needsRepaint()
{
    return pimpl->needsRepaint();
}

bool Direct2DLowLevelGraphicsContext::startPartialAsynchronousPaint(int frameNumber)
{
    juce::Rectangle<int> initialClipBounds;
    if (pimpl->startRender(frameNumber, initialClipBounds))
    {
        saveState();

        if (initialClipBounds.isEmpty() == false)
        {
            //DBG("   deferredRepaintsBounds " << deferredRepaintsBounds.toString());
            clipToRectangle(initialClipBounds);
            //DBG("      clipRegion " << currentState->clipRegion.toString());
        }

        return true;
    }

    return false;
}

void Direct2DLowLevelGraphicsContext::end()
{
    while (states.size() > 0)
    {
        states.removeLast(1);
    }
    currentState = nullptr;

    pimpl->finishRender();

    pimpl->updateRegion.clear();
}

void Direct2DLowLevelGraphicsContext::setOrigin (Point<int> o)
{
    currentState->currentTransform.setOrigin(o);
}

void Direct2DLowLevelGraphicsContext::addTransform (const AffineTransform& transform)
{
    currentState->currentTransform.addTransform(transform);
}

float Direct2DLowLevelGraphicsContext::getPhysicalPixelScaleFactor()
{
    return currentState->currentTransform.getPhysicalPixelScaleFactor();
}

bool Direct2DLowLevelGraphicsContext::clipToRectangle (const Rectangle<int>& r)
{
    //
    // Update the current clip region (only used for getClipBounds)
    //
    auto currentTransform = currentState->currentTransform.getTransform();
    auto transformedR = r.transformedBy(currentTransform);
    transformedR.intersectRectangle(currentState->clipRegion);

    //
    // Push a clip layer
    //
    if (direct2d::isTransformOnlyTranslationOrScale(currentTransform))
    {
        //
        // If the clip rectangle will still be a rectangle with vertical and horizontal sides after transformation, then use an axis-aligned clip layer
        //
        currentState->pushAxisAlignedClipLayer(transformedR);
    }
    else
    {
        //
        // If the current transform is nontrivial (shear, rotation, etc), then use a transformed geometry for the clip layer
        //
        currentState->pushGeometryClipLayer(pimpl->rectToPathGeometry(r, currentTransform, D2D1_FILL_MODE_WINDING));
    }

    return ! isClipEmpty();
}

bool Direct2DLowLevelGraphicsContext::clipToRectangleList (const RectangleList<int>& clipRegion)
{
    //
    // Update the current clip region (only used for getClipBounds)
    //
    auto const currentTransform = currentState->currentTransform.getTransform();
    auto transformedR = clipRegion.getBounds().transformedBy(currentTransform);
    transformedR.intersectRectangle(currentState->clipRegion);

    currentState->pushGeometryClipLayer(pimpl->rectListToPathGeometry(clipRegion, currentState->currentTransform.getTransform(), D2D1_FILL_MODE_WINDING));

    return ! isClipEmpty();
}

void Direct2DLowLevelGraphicsContext::excludeClipRectangle (const Rectangle<int>& r)
{
    //
    // To exclude the rectangle r, build a rectangle list with r as the first rectangle and the render target bounds as the second.
    // 
    // Then, convert that rectangle list to a geometry, but specify D2D1_FILL_MODE_ALTERNATE so the inside of r is *outside*
    // the geometry and everything else on the screen is inside the geometry.
    // 
    // Have to use addWithoutMerging to build the rectangle list to keep the rectangles separate.
    //
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        RectangleList<int> rectangles{ r };
        auto size = deviceContext->GetPixelSize();
        rectangles.addWithoutMerging({ 0, 0, (int)size.width, (int)size.height });

        currentState->pushGeometryClipLayer(pimpl->rectListToPathGeometry(rectangles, currentState->currentTransform.getTransform(), D2D1_FILL_MODE_ALTERNATE));
    }
}

void Direct2DLowLevelGraphicsContext::clipToPath (const Path& path, const AffineTransform& transform)
{
    currentState->pushGeometryClipLayer(pimpl->pathToPathGeometry(path, currentState->currentTransform.getTransformWith(transform)));
}

void Direct2DLowLevelGraphicsContext::clipToImageAlpha (const Image& sourceImage, const AffineTransform& transform)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        auto chainedTransform = currentState->currentTransform.getTransformWith(transform);
        auto transformedR = sourceImage.getBounds().transformedBy(chainedTransform);
        transformedR.intersectRectangle(currentState->clipRegion);

        auto maskImage = sourceImage.convertedToFormat(Image::ARGB);

        ComSmartPtr<ID2D1Bitmap> bitmap;
        ComSmartPtr<ID2D1BitmapBrush> brush;

        D2D1_BRUSH_PROPERTIES brushProps = { 1, direct2d::transformToMatrix(chainedTransform) };
        auto bmProps = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP);
        auto bp = D2D1::BitmapProperties();

        Image::BitmapData bd{ maskImage, Image::BitmapData::readOnly };
        bp.pixelFormat = deviceContext->GetPixelFormat();
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

        auto hr = deviceContext->CreateBitmap(D2D1_SIZE_U{ (UINT32)maskImage.getWidth(), (UINT32)maskImage.getHeight() }, bd.data, bd.lineStride, bp, bitmap.resetAndGetPointerAddress());
        hr = deviceContext->CreateBitmapBrush(bitmap, bmProps, brushProps, brush.resetAndGetPointerAddress());

        auto layerParams = D2D1::LayerParameters();
        layerParams.opacityBrush = brush;

        currentState->pushLayer(layerParams);
    }
}

bool Direct2DLowLevelGraphicsContext::clipRegionIntersects (const Rectangle<int>& r)
{
    return getClipBounds().intersects(r);
}

Rectangle<int> Direct2DLowLevelGraphicsContext::getClipBounds() const
{
    return currentState->currentTransform.deviceSpaceToUserSpace(currentState->clipRegion);
}

bool Direct2DLowLevelGraphicsContext::isClipEmpty() const
{
    return getClipBounds().isEmpty();
}

void Direct2DLowLevelGraphicsContext::saveState()
{
    states.add (new SavedState (*this));
    currentState = states.getLast();
}

void Direct2DLowLevelGraphicsContext::restoreState()
{
    jassert (states.size() > 1); //you should never pop the last state!
    states.removeLast (1);
    currentState = states.getLast();

    //
    // The solid color brush is shared between states, so restore the previous solid color and opacity
    //
    currentState->updateColourBrush();
}

void Direct2DLowLevelGraphicsContext::beginTransparencyLayer(float opacity)
{
    currentState->beginTransparency(opacity);
}

void Direct2DLowLevelGraphicsContext::endTransparencyLayer()
{
    //
    // Do nothing; the Direct2D transparency layer will be popped along with the current saved state
    //
}

void Direct2DLowLevelGraphicsContext::setFill (const FillType& fillType)
{
    currentState->setFill (fillType);
}

void Direct2DLowLevelGraphicsContext::setOpacity (float newOpacity)
{
    currentState->setOpacity (newOpacity);
}

void Direct2DLowLevelGraphicsContext::setInterpolationQuality (Graphics::ResamplingQuality quality)
{
    switch (quality)
    {
    case Graphics::ResamplingQuality::lowResamplingQuality:
        currentState->interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        break;

    case Graphics::ResamplingQuality::mediumResamplingQuality:
        currentState->interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;
        break;

    case Graphics::ResamplingQuality::highResamplingQuality:
        currentState->interpolationMode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
        break;
    }
}

void Direct2DLowLevelGraphicsContext::fillRect (const Rectangle<int>& r, bool /*replaceExistingContents*/)
{
    fillRect (r.toFloat());
}

void Direct2DLowLevelGraphicsContext::fillRect (const Rectangle<float>& r)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
        deviceContext->FillRectangle(direct2d::rectangleToRectF(r), currentState->currentBrush);
        deviceContext->SetTransform(D2D1::IdentityMatrix());
    }
}

void Direct2DLowLevelGraphicsContext::fillRectList (const RectangleList<float>& list)
{
    for (auto& r : list)
        fillRect (r);
}

bool Direct2DLowLevelGraphicsContext::drawRect(const Rectangle<float>& r, float lineThickness)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
        deviceContext->DrawRectangle(direct2d::rectangleToRectF(r), currentState->currentBrush, lineThickness);
        deviceContext->SetTransform(D2D1::IdentityMatrix());

        return true;
    }

    return false;
}

void Direct2DLowLevelGraphicsContext::fillPath (const Path& p, const AffineTransform& transform)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        if (auto geometry = pimpl->pathToPathGeometry(p, transform))
        {
            deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
            deviceContext->FillGeometry(geometry, currentState->currentBrush);
            deviceContext->SetTransform(D2D1::IdentityMatrix());
        }
    }
}

bool Direct2DLowLevelGraphicsContext::drawPath(const Path& p, const PathStrokeType& strokeType, const AffineTransform& transform)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        if (auto geometry = pimpl->pathToPathGeometry(p, transform))
        {
            // JUCE JointStyle   ID2D1StrokeStyle 
            // ---------------   ----------------
            // mitered           D2D1_LINE_JOIN_MITER
            // curved            D2D1_LINE_JOIN_ROUND
            // beveled           D2D1_LINE_JOIN_BEVEL
            //
            // JUCE EndCapStyle  ID2D1StrokeStyle 
            // ----------------  ----------------
            // butt              D2D1_CAP_STYLE_FLAT
            // square            D2D1_CAP_STYLE_SQUARE 
            // rounded           D2D1_CAP_STYLE_ROUND
            //
            auto lineJoin = D2D1_LINE_JOIN_MITER;
            switch (strokeType.getJointStyle())
            {
            case PathStrokeType::JointStyle::mitered:
                // already set
                break;

            case PathStrokeType::JointStyle::curved:
                lineJoin = D2D1_LINE_JOIN_ROUND;
                break;

            case PathStrokeType::JointStyle::beveled:
                lineJoin = D2D1_LINE_JOIN_BEVEL;
                break;

            default:
                // invalid EndCapStyle
                jassertfalse;
                break;
            }

            auto capStyle = D2D1_CAP_STYLE_FLAT;
            switch (strokeType.getEndStyle())
            {
            case PathStrokeType::EndCapStyle::butt:
                // already set
                break;

            case PathStrokeType::EndCapStyle::square:
                capStyle = D2D1_CAP_STYLE_SQUARE;
                break;

            case PathStrokeType::EndCapStyle::rounded:
                capStyle = D2D1_CAP_STYLE_ROUND;
                break;

            default:
                // invalid EndCapStyle
                jassertfalse;
                break;
            }

            D2D1_STROKE_STYLE_PROPERTIES strokeStyleProperties
            {
                capStyle, capStyle, capStyle,
                lineJoin,
                1.0f,
                D2D1_DASH_STYLE_SOLID,
                0.0f
            };
            pimpl->d2dDedicatedFactory->CreateStrokeStyle(strokeStyleProperties, // TODO reuse the stroke style
                nullptr, 0,
                pimpl->strokeStyle.resetAndGetPointerAddress());

            deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
            deviceContext->DrawGeometry(geometry, currentState->currentBrush, strokeType.getStrokeThickness(), pimpl->strokeStyle);
            deviceContext->SetTransform(D2D1::IdentityMatrix());

            return true;
        }
    }

    return false;
}

void Direct2DLowLevelGraphicsContext::drawImage (const Image& image, const AffineTransform& transform)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransformWith(transform)));

        D2D1_SIZE_U size = { (UINT32)image.getWidth(), (UINT32)image.getHeight() };
        auto bp = D2D1::BitmapProperties();

        Image img(image.convertedToFormat(Image::ARGB));
        Image::BitmapData bd(img, Image::BitmapData::readOnly);
        bp.pixelFormat = deviceContext->GetPixelFormat();
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

        {
            ComSmartPtr<ID2D1Bitmap> tempBitmap;
            deviceContext->CreateBitmap(size, bd.data, bd.lineStride, bp, tempBitmap.resetAndGetPointerAddress());
            if (tempBitmap != nullptr)
            {
                deviceContext->DrawImage(tempBitmap, currentState->interpolationMode);
            }
        }

        deviceContext->SetTransform(D2D1::IdentityMatrix());
    }
}

void Direct2DLowLevelGraphicsContext::drawLine (const Line<float>& line)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
        currentState->createBrush();

        deviceContext->DrawLine(D2D1::Point2F(line.getStartX(), line.getStartY()),
            D2D1::Point2F(line.getEndX(), line.getEndY()),
            currentState->currentBrush);
        deviceContext->SetTransform(D2D1::IdentityMatrix());
    }
}

void Direct2DLowLevelGraphicsContext::setFont (const Font& newFont)
{
    currentState->setFont (newFont);
}

const Font& Direct2DLowLevelGraphicsContext::getFont()
{
    return currentState->font;
}

void Direct2DLowLevelGraphicsContext::drawGlyph (int glyphNumber, const AffineTransform& transform)
{
    currentState->createBrush();
    currentState->createFont();

    jassert(currentState->currentFontFace);

    auto deviceContext = pimpl->getDeviceContext();
    if (currentState->currentFontFace != nullptr && deviceContext != nullptr)
    {
        auto hScale = currentState->font.getHorizontalScale();
        auto scaledTransform = AffineTransform::scale(hScale, 1.0f).followedBy(transform);
        auto deviceContextTransform = scaledTransform.followedBy(currentState->currentTransform.getTransform());
        deviceContext->SetTransform(direct2d::transformToMatrix(deviceContextTransform));

        const auto glyphIndices = (UINT16)glyphNumber;
        const auto glyphAdvances = 0.0f;
        DWRITE_GLYPH_OFFSET offset = { 0.0f, 0.0f };

        DWRITE_GLYPH_RUN glyphRun;
        glyphRun.fontFace = currentState->currentFontFace;
        glyphRun.fontEmSize = (FLOAT)(currentState->font.getHeight() * currentState->fontHeightToEmSizeFactor);
        glyphRun.glyphCount = 1;
        glyphRun.glyphIndices = &glyphIndices;
        glyphRun.glyphAdvances = &glyphAdvances;
        glyphRun.glyphOffsets = &offset;
        glyphRun.isSideways = FALSE;
        glyphRun.bidiLevel = 0;

        //
        // The gradient brushes are position-dependent, so need to undo the device context transform
        //
        D2D1::Matrix3x2F brushTransform;
        currentState->currentBrush->GetTransform(&brushTransform);
        currentState->currentBrush->SetTransform(direct2d::transformToMatrix(scaledTransform.inverted()));

        deviceContext->DrawGlyphRun({}, &glyphRun, currentState->currentBrush);

        deviceContext->SetTransform(D2D1::IdentityMatrix());
        currentState->currentBrush->SetTransform(brushTransform);
    }
}

bool Direct2DLowLevelGraphicsContext::drawTextLayout (const AttributedString& text, const Rectangle<float>& area)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));

        DirectWriteTypeLayout::drawToD2DContext(text, area,
            *(deviceContext),
            *(pimpl->sharedFactories->directWriteFactory),
            *(pimpl->sharedFactories->systemFonts));

        deviceContext->SetTransform(D2D1::IdentityMatrix());
    }

    return true;
}

void Direct2DLowLevelGraphicsContext::setScaleFactor(double scale_)
{
    pimpl->setScaleFactor(scale_);
}

double Direct2DLowLevelGraphicsContext::getScaleFactor() const
{
    return pimpl->getScaleFactor();
}

bool Direct2DLowLevelGraphicsContext::drawRoundedRectangle(Rectangle<float> area, float cornerSize, float lineThickness)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));

        D2D1_ROUNDED_RECT roundedRect
        {
            direct2d::rectangleToRectF(area),
            cornerSize, cornerSize
        };
        deviceContext->DrawRoundedRectangle(roundedRect, currentState->currentBrush, lineThickness);
        deviceContext->SetTransform(D2D1::IdentityMatrix());

        return true;
    }

    return false;
}

bool Direct2DLowLevelGraphicsContext::fillRoundedRectangle(Rectangle<float> area, float cornerSize)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));
        
        D2D1_ROUNDED_RECT roundedRect
        {
            direct2d::rectangleToRectF(area),
            cornerSize, cornerSize
        };
        deviceContext->FillRoundedRectangle(roundedRect, currentState->currentBrush);
        deviceContext->SetTransform(D2D1::IdentityMatrix());

        return true;
    }

    return false;
}

bool Direct2DLowLevelGraphicsContext::drawEllipse(Rectangle<float> area, float lineThickness)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));

        D2D1_ELLIPSE ellipse
        {
            { area.getCentreX(), area.getCentreY() },
            area.proportionOfWidth(0.5f), area.proportionOfHeight(0.5f)
        };
        deviceContext->DrawEllipse(ellipse, currentState->currentBrush, lineThickness, nullptr);
        deviceContext->SetTransform(D2D1::IdentityMatrix());

        return true;
    }

    return false;
}

bool Direct2DLowLevelGraphicsContext::fillEllipse(Rectangle<float> area)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(direct2d::transformToMatrix(currentState->currentTransform.getTransform()));

        D2D1_ELLIPSE ellipse
        {
            { area.getCentreX(), area.getCentreY() },
            area.proportionOfWidth(0.5f), area.proportionOfHeight(0.5f)
        };
        deviceContext->FillEllipse(ellipse, currentState->currentBrush);
        deviceContext->SetTransform(D2D1::IdentityMatrix());

        return true;
    }

    return false;
}

void Direct2DLowLevelGraphicsContext::handleAsyncUpdate()
{
    if (onPaintReady)
    {
        onPaintReady();
    }
}

#if 0 // JUCE_DEBUG
void printTransform(StringRef name, AffineTransform const& transform)
{
    DBG(name << "  scale:" << transform.getScaleFactor() << "  translate:" << transform.getTranslationX() << " / " << transform.getTranslationY());
}
#endif

void Direct2DLowLevelGraphicsContext::drawGlyphRun(Array<Glyph> const& glyphRun, const AffineTransform& transform)
{
    currentState->createBrush();
    currentState->createFont();

    jassert(currentState->currentFontFace);

#if 0
    DBG("drawGlyphRun");

    for (auto const& glyph : glyphRun)
    {
        DBG("glyph " << glyph.glyphIndex << " x:" << glyph.left << " / " << glyph.baselineY);
    }
#endif

    auto deviceContext = pimpl->getDeviceContext();
    if (currentState->currentFontFace != nullptr && deviceContext != nullptr && glyphRun.size() > 0)
    {
        auto hScale = currentState->font.getHorizontalScale();
        auto inverseHScale = hScale > 0.0f ? 1.0f / hScale : 1.0f;

        auto scaledTransform = AffineTransform::scale(hScale, 1.0f).followedBy(transform);
        auto deviceContextTransform = scaledTransform.followedBy(currentState->currentTransform.getTransform());
        deviceContext->SetTransform(direct2d::transformToMatrix(deviceContextTransform));

        //printTransform("   deviceContextTransform ", deviceContextTransform);

        HeapBlock<UINT16> glyphIndices{ glyphRun.size() };
        HeapBlock<float> glyphAdvances{ glyphRun.size() };
        HeapBlock<DWRITE_GLYPH_OFFSET> glyphOffsets{ glyphRun.size() };

        for (int i = 0; i < glyphRun.size(); ++i)
        {
            auto const& glyph = glyphRun[i];
            glyphIndices[i] = (UINT16)glyph.glyphIndex;
            glyphAdvances[i] = 0.0f;
            glyphOffsets[i] = { glyph.left * inverseHScale, -glyph.baselineY }; // note the essential minus sign before the baselineY value; negatvie offset goes down, positive goes up (opposite from JUCE)
        }

        DWRITE_GLYPH_RUN directWriteGlyphRun;
        directWriteGlyphRun.fontFace = currentState->currentFontFace;
        directWriteGlyphRun.fontEmSize = (FLOAT)(currentState->font.getHeight() * currentState->fontHeightToEmSizeFactor);
        directWriteGlyphRun.glyphCount = glyphRun.size();
        directWriteGlyphRun.glyphIndices = glyphIndices.getData();
        directWriteGlyphRun.glyphAdvances = glyphAdvances.getData();
        directWriteGlyphRun.glyphOffsets = glyphOffsets.getData();
        directWriteGlyphRun.isSideways = FALSE;
        directWriteGlyphRun.bidiLevel = 0;

        //
        // The gradient brushes are position-dependent, so need to undo the device context transform
        //
        D2D1::Matrix3x2F brushTransform;
        currentState->currentBrush->GetTransform(&brushTransform);
        currentState->currentBrush->SetTransform(direct2d::transformToMatrix(scaledTransform.inverted()));

        deviceContext->DrawGlyphRun({}, &directWriteGlyphRun, currentState->currentBrush);

        deviceContext->SetTransform(D2D1::IdentityMatrix());
        currentState->currentBrush->SetTransform(brushTransform);
    }
}

} // namespace juce
