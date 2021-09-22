/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 6 End-User License
   Agreement and JUCE Privacy Policy (both effective as of the 16th June 2020).

   End User License Agreement: www.juce.com/juce-6-licence
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

template <typename Type>
D2D1_RECT_F rectangleToRectF (const Rectangle<Type>& r)
{
    return { (float) r.getX(), (float) r.getY(), (float) r.getRight(), (float) r.getBottom() };
}

static D2D1_COLOR_F colourToD2D (Colour c)
{
    return { c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), c.getFloatAlpha() };
}

static bool isTransformOnlyTranslationOrScale(AffineTransform const& transform)
{
    return transform.mat01 == 0.0f;
}

static void pathToGeometrySink (const Path& path, ID2D1GeometrySink* sink, const AffineTransform& transform)
{
    //
    // Every call to BeginFigure must have a matching call to EndFigure. But - the Path does not necessarily
    // have matching startNewSubPath and closePath markers. The figureStarted flag indicates if an extra call
    // to BeginFigure or EndFigure is needed during the iteration loop or when exiting this function.
    //
    Path::Iterator it (path);
    bool figureStarted = false;

    while (it.next())
    {
        switch (it.elementType)
        {
        case Path::Iterator::cubicTo:
        {
            jassert(figureStarted);

            transform.transformPoint (it.x1, it.y1);
            transform.transformPoint (it.x2, it.y2);
            transform.transformPoint (it.x3, it.y3);

            sink->AddBezier ({ { it.x1, it.y1 }, { it.x2, it.y2 }, { it.x3, it.y3 } });
            break;
        }

        case Path::Iterator::lineTo:
        {
            jassert(figureStarted);

            transform.transformPoint (it.x1, it.y1);
            sink->AddLine ({ it.x1, it.y1 });
            break;
        }

        case Path::Iterator::quadraticTo:
        {
            jassert(figureStarted);

            transform.transformPoint (it.x1, it.y1);
            transform.transformPoint (it.x2, it.y2);
            sink->AddQuadraticBezier ({ { it.x1, it.y1 }, { it.x2, it.y2 } });
            break;
        }

        case Path::Iterator::closePath:
        {
            if (figureStarted)
            {
	            sink->EndFigure (D2D1_FIGURE_END_CLOSED);
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

            transform.transformPoint (it.x1, it.y1);
            sink->BeginFigure ({ it.x1, it.y1 }, D2D1_FIGURE_BEGIN_FILLED);
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

static D2D1::Matrix3x2F transformToMatrix (const AffineTransform& transform)
{
    return { transform.mat00, transform.mat10, transform.mat01, transform.mat11, transform.mat02, transform.mat12 };
}

static D2D1_POINT_2F pointTransformed (int x, int y, const AffineTransform& transform)
{
    transform.transformPoint (x, y);
    return { (FLOAT) x, (FLOAT) y };
}

static void rectToGeometrySink (const Rectangle<int>& rect, ID2D1GeometrySink* sink, const AffineTransform& transform)
{
    sink->BeginFigure (pointTransformed (rect.getX(),     rect.getY(),       transform), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine     (pointTransformed (rect.getRight(), rect.getY(),       transform));
    sink->AddLine     (pointTransformed (rect.getRight(), rect.getBottom(),  transform));
    sink->AddLine     (pointTransformed (rect.getX(),     rect.getBottom(),  transform));
    sink->EndFigure (D2D1_FIGURE_END_CLOSED);
}

//==============================================================================
struct Direct2DLowLevelGraphicsContext::Pimpl
{
    Pimpl(HWND hwnd_) :
        hwnd(hwnd_)
    {
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
        ScopedGeometryWithSink objects{ factories->d2dFactory, fillMode };

        if (objects.sink != nullptr)
        {
            rectToGeometrySink(rect, objects.sink, transform);
            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ComSmartPtr<ID2D1Geometry> rectListToPathGeometry (const RectangleList<int>& clipRegion, const AffineTransform& transform, D2D1_FILL_MODE fillMode)
    {
        ScopedGeometryWithSink objects{ factories->d2dFactory, fillMode };

        if (objects.sink != nullptr)
        {
            for (int i = clipRegion.getNumRectangles(); --i >= 0;)
                rectToGeometrySink(clipRegion.getRectangle(i), objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ComSmartPtr<ID2D1Geometry> pathToPathGeometry (const Path& path, const AffineTransform& transform)
    {
        ScopedGeometryWithSink objects{ factories->d2dFactory, path.isUsingNonZeroWinding() ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE };

        if (objects.sink != nullptr)
        {
            pathToGeometrySink(path, objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    void createDeviceContext()
    {
        if (factories->d2dFactory != nullptr)
        {
            if (renderingTarget == nullptr)
            {
                // This flag adds support for surfaces with a different color channel ordering
                // than the API default. It is required for compatibility with Direct2D.
                UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if JUCE_DEBUG
                creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

                juce::ComSmartPtr<ID3D11Device> direct3DDevice;
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
                    juce::ComSmartPtr<IDXGIDevice> dxgiDevice;
                    hr = direct3DDevice->QueryInterface(dxgiDevice.resetAndGetPointerAddress());
                    if (SUCCEEDED(hr))
                    {
                        juce::ComSmartPtr<IDXGIAdapter> dxgiAdapter;
                        hr = dxgiDevice->GetAdapter(dxgiAdapter.resetAndGetPointerAddress());
                        if (SUCCEEDED(hr))
                        {
                            juce::ComSmartPtr<IDXGIFactory2> dxgiFactory;
                            hr = dxgiAdapter->GetParent(__uuidof(dxgiFactory), reinterpret_cast<void**>(dxgiFactory.resetAndGetPointerAddress()));
                            if (SUCCEEDED(hr))
                            {
                                DXGI_SWAP_CHAIN_DESC1 swapChainDescription = {};
                                swapChainDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                swapChainDescription.SampleDesc.Count = 1;
                                swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                swapChainDescription.BufferCount = 2;
                                swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
                                hr = dxgiFactory->CreateSwapChainForHwnd(direct3DDevice,
                                    hwnd,
                                    &swapChainDescription,
                                    nullptr,
                                    nullptr,
                                    swapChain.resetAndGetPointerAddress());
                                if (SUCCEEDED(hr))
                                {
                                    juce::ComSmartPtr<ID2D1Device> direct2DDevice;
                                    hr = factories->d2dFactory->CreateDevice(dxgiDevice, direct2DDevice.resetAndGetPointerAddress());
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = direct2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, renderingTarget.resetAndGetPointerAddress());
                                    }
                                }
                            }
                        }
                    }
                }
                jassert(SUCCEEDED(hr));
            }

            if (colourBrush == nullptr && renderingTarget != nullptr)
            {
                auto hr = renderingTarget->CreateSolidColorBrush(D2D1::ColorF::ColorF(0.0f, 0.0f, 0.0f, 1.0f), colourBrush.resetAndGetPointerAddress());
                jassert(SUCCEEDED(hr));
            }
        }
    }

    void releaseDeviceContext()
    {
        colourBrush = nullptr;
        swapChainBuffer = nullptr;
        swapChain = nullptr;
        renderingTarget = nullptr;
    }

    void createSwapChainBuffer()
    {
        if (renderingTarget != nullptr && swapChain != nullptr && swapChainBuffer == nullptr)
        {
            juce::ComSmartPtr<IDXGISurface> surface;
            auto hr = swapChain->GetBuffer(0, __uuidof(surface), reinterpret_cast<void**>(surface.resetAndGetPointerAddress()));
            if (SUCCEEDED(hr))
            {
                D2D1_BITMAP_PROPERTIES1 bitmapProperties = {};
                bitmapProperties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                bitmapProperties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                bitmapProperties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
                hr = renderingTarget->CreateBitmapFromDxgiSurface(surface, bitmapProperties, swapChainBuffer.resetAndGetPointerAddress());
                jassert(SUCCEEDED(hr));
#if 0 // xxx how to handle DPI scaling for Windows 8?
                if (SUCCEEDED(hr))
                {
                    UINT GetDpiForWindow(HWND hwnd);
                    auto dpi = GetDpiForWindow(hwnd);
                    renderingTarget->SetDpi((float)dpi, (float)dpi);
                }
#endif
            }
        }
    }

    SharedResourcePointer<Direct2DFactories> factories;

    HWND hwnd = nullptr;
    juce::ComSmartPtr<ID2D1DeviceContext> renderingTarget;
    juce::ComSmartPtr<IDXGISwapChain1> swapChain;
    juce::ComSmartPtr<ID2D1Bitmap1> swapChainBuffer;
    ComSmartPtr<ID2D1SolidColorBrush> colourBrush;
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
            clipRect = owner.currentState->clipRect;
            transform = owner.currentState->transform;

            font = owner.currentState->font;
            currentFontFace = owner.currentState->currentFontFace;
        }
        else
        {
            if (owner.pimpl->renderingTarget != nullptr)
            {
                const auto size = owner.pimpl->renderingTarget->GetPixelSize();
                clipRect.setSize(size.width, size.height);
            }
            setFill (FillType (Colours::black));
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
        if (owner.pimpl->renderingTarget != nullptr)
        {
	        //
	        // Clipping and transparency are all handled by pushing Direct2D layers. The SavedState creates an internal stack
	        // of Layer objects to keep track of how many layers need to be popped.
	        // 
	        // Pass nullptr for the layer to allow Direct2D to manage the layers (Windows 8 or later)
	        //
	        owner.pimpl->renderingTarget->PushLayer(layerParameters, nullptr);
	
	        pushedLayers.add(new Layer{ owner.pimpl->renderingTarget });
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
        if (owner.pimpl->renderingTarget != nullptr)
        {
            owner.pimpl->renderingTarget->PushAxisAlignedClip(rectangleToRectF(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            pushedLayers.add(new AxisAlignedClipLayer{ owner.pimpl->renderingTarget });
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
            auto* typeface = dynamic_cast<WindowsDirectWriteTypeface*> (font.getTypeface());
            if (typeface)
            {
                currentFontFace = typeface->getIDWriteFontFace();
                fontHeightToEmSizeFactor = typeface->getUnitsToHeightScaleFactor();
            }
        }
    }

    void setOpacity (float newOpacity)
    {
        fillType.setOpacity (newOpacity);

        if (currentBrush != nullptr)
            currentBrush->SetOpacity (newOpacity);
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
        if (currentBrush == nullptr && owner.pimpl->renderingTarget != nullptr)
        {
            if (fillType.isColour())
            {
                updateColourBrush();

                currentBrush = owner.pimpl->colourBrush;
            }
            else if (fillType.isTiledImage())
            {
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), transformToMatrix (fillType.transform) };
                auto bmProps = D2D1::BitmapBrushProperties (D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP);

                auto image = fillType.image;

                D2D1_SIZE_U size = { (UINT32) image.getWidth(), (UINT32) image.getHeight() };
                auto bp = D2D1::BitmapProperties();

                image = image.convertedToFormat (Image::ARGB);
                Image::BitmapData bd (image, Image::BitmapData::readOnly);
                bp.pixelFormat = owner.pimpl->renderingTarget->GetPixelFormat();
                bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

                ComSmartPtr<ID2D1Bitmap> tiledImageBitmap;
                auto hr = owner.pimpl->renderingTarget->CreateBitmap (size, bd.data, bd.lineStride, bp, tiledImageBitmap.resetAndGetPointerAddress());
                jassert(SUCCEEDED(hr));
                if (SUCCEEDED(hr))
                {
                    hr = owner.pimpl->renderingTarget->CreateBitmapBrush(tiledImageBitmap, bmProps, brushProps, bitmapBrush.resetAndGetPointerAddress());
                    jassert(SUCCEEDED(hr));
                    if (SUCCEEDED(hr))
                    {
                        currentBrush = bitmapBrush;
                    }
                }
            }
            else if (fillType.isGradient())
            {
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), transformToMatrix (fillType.transform.followedBy(transform)) };

                const int numColors = fillType.gradient->getNumColours();

                HeapBlock<D2D1_GRADIENT_STOP> stops (numColors);

                for (int i = fillType.gradient->getNumColours(); --i >= 0;)
                {
                    stops[i].color = colourToD2D (fillType.gradient->getColour (i));
                    stops[i].position = (FLOAT) fillType.gradient->getColourPosition (i);
                }

                owner.pimpl->renderingTarget->CreateGradientStopCollection (stops.getData(), numColors, gradientStops.resetAndGetPointerAddress());

                if (fillType.gradient->isRadial)
                {
                    const auto p1 = fillType.gradient->point1;
                    const auto p2 = fillType.gradient->point2;
                    const auto r = p1.getDistanceFrom(p2);
                    const auto props = D2D1::RadialGradientBrushProperties ({ p1.x, p1.y }, {}, r, r);

                    owner.pimpl->renderingTarget->CreateRadialGradientBrush (props, brushProps, gradientStops, radialGradient.resetAndGetPointerAddress());
                    currentBrush = radialGradient;
                }
                else
                {
                    const auto p1 = fillType.gradient->point1;
                    const auto p2 = fillType.gradient->point2;
                    const auto props = D2D1::LinearGradientBrushProperties ({ p1.x, p1.y }, { p2.x, p2.y });

                    owner.pimpl->renderingTarget->CreateLinearGradientBrush (props, brushProps, gradientStops, linearGradient.resetAndGetPointerAddress());

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
        if (owner.pimpl->colourBrush != nullptr)
        {
            auto colour = colourToD2D(fillType.colour);
            owner.pimpl->colourBrush->SetColor(colour);
        }
    }

    Direct2DLowLevelGraphicsContext& owner;

    AffineTransform transform;

    Font font;
    float fontHeightToEmSizeFactor = 1.0f;

    IDWriteFontFace* currentFontFace = nullptr;
    ComSmartPtr<IDWriteFontFace> localFontFace;

    Rectangle<int> clipRect;

    //
    // Layer struct to keep track of pushed Direct2D layers.
    // 
    // Most layers need to be popped by calling PopLayer, unless it's an axis aligned clip layer
    //
    struct Layer
    {
        Layer(ComSmartPtr<ID2D1DeviceContext>& renderingTarget_) :
            renderingTarget(renderingTarget_)
        {
            jassert(renderingTarget_ != nullptr);
        }
        virtual ~Layer() = default;

        virtual void pop()
        {
            renderingTarget->PopLayer();
        }

        ComSmartPtr<ID2D1DeviceContext> renderingTarget;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Layer)
    };

    struct AxisAlignedClipLayer : public Layer
    {
        AxisAlignedClipLayer(ComSmartPtr<ID2D1DeviceContext>& renderingTarget_) :
            Layer(renderingTarget_)
        {
        }
        ~AxisAlignedClipLayer() override = default;

        virtual void pop()
        {
            renderingTarget->PopAxisAlignedClip();
        }
    };

    OwnedArray<Layer> pushedLayers;

    ID2D1Brush* currentBrush = nullptr;
    ComSmartPtr<ID2D1BitmapBrush> bitmapBrush;
    ComSmartPtr<ID2D1LinearGradientBrush> linearGradient;
    ComSmartPtr<ID2D1RadialGradientBrush> radialGradient;
    ComSmartPtr<ID2D1GradientStopCollection> gradientStops;

    FillType fillType;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SavedState)
};

//==============================================================================
Direct2DLowLevelGraphicsContext::Direct2DLowLevelGraphicsContext (HWND hwnd_)
    : currentState (nullptr),
      pimpl (new Pimpl(hwnd_))
{
    RECT windowRect;
    GetClientRect(hwnd_, &windowRect);
    D2D1_SIZE_U size = { (UINT32)(windowRect.right - windowRect.left), (UINT32)(windowRect.bottom - windowRect.top) };
    bounds.setSize(size.width, size.height);
}

Direct2DLowLevelGraphicsContext::~Direct2DLowLevelGraphicsContext()
{
    states.clear();
}

void Direct2DLowLevelGraphicsContext::resized()
{
    if (pimpl->renderingTarget != nullptr)
    {
        pimpl->renderingTarget->SetTarget(nullptr);
    }

    if (pimpl->swapChain != nullptr)
    {
        pimpl->swapChainBuffer = nullptr; // must release swap chain buffer before calling ResizeBuffers

	    RECT windowRect;
	    GetClientRect (pimpl->hwnd, &windowRect);
        auto width = windowRect.right - windowRect.left;
        auto height = windowRect.bottom - windowRect.top;
	
        auto hr = pimpl->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr))
        {
            pimpl->createSwapChainBuffer();
        }
        else
        {
            pimpl->releaseDeviceContext();
        }

	    bounds.setSize (width, height);
    }
}

void Direct2DLowLevelGraphicsContext::start()
{
    pimpl->createDeviceContext();
    if (pimpl->renderingTarget != nullptr)
    {
        pimpl->createSwapChainBuffer();
        if (pimpl->swapChainBuffer != nullptr)
        {
            pimpl->renderingTarget->SetTarget(pimpl->swapChainBuffer);
            pimpl->renderingTarget->BeginDraw();
        }
    }

    saveState();
}

void Direct2DLowLevelGraphicsContext::end()
{
    while (states.size() > 0)
    {
        states.removeLast(1);
    }
    currentState = nullptr;

    if (pimpl->renderingTarget != nullptr && pimpl->swapChain != nullptr)
    {
        auto hr = pimpl->renderingTarget->EndDraw();
        if (SUCCEEDED(hr))
        {
            hr = pimpl->swapChain->Present(1, D2D1_PRESENT_OPTIONS_NONE);
        }

        if (S_OK != hr && DXGI_STATUS_OCCLUDED != hr)
        {
            pimpl->releaseDeviceContext();
        }
    }
}

void Direct2DLowLevelGraphicsContext::setOrigin (Point<int> o)
{
    currentState->clipRect.setPosition(currentState->clipRect.getTopLeft() - o);

    addTransform (AffineTransform::translation ((float) o.x, (float) o.y));
}

void Direct2DLowLevelGraphicsContext::addTransform (const AffineTransform& transform)
{
    currentState->transform = transform.followedBy (currentState->transform);
}

float Direct2DLowLevelGraphicsContext::getPhysicalPixelScaleFactor()
{
    return std::sqrt (std::abs (currentState->transform.getDeterminant()));
}

bool Direct2DLowLevelGraphicsContext::clipToRectangle (const Rectangle<int>& r)
{
    currentState->clipRect = r;

    if (isTransformOnlyTranslationOrScale(currentState->transform))
    {
        //
        // If the current transform is just a translation, use an axis-aligned clip layer (according to the Direct2D debug layer)
        //
        currentState->pushAxisAlignedClipLayer(r.transformedBy(currentState->transform));
    }
    else
    {
        //
        // If the current transform is nontrivial (shear, rotation, etc), then use a transformed geometry for the clip layer
        //
        currentState->pushGeometryClipLayer(pimpl->rectToPathGeometry(r, currentState->transform, D2D1_FILL_MODE_WINDING));
    }

    return ! isClipEmpty();
}

bool Direct2DLowLevelGraphicsContext::clipToRectangleList (const RectangleList<int>& clipRegion)
{
    currentState->clipRect = clipRegion.getBounds();

    currentState->pushGeometryClipLayer(pimpl->rectListToPathGeometry(clipRegion, currentState->transform, D2D1_FILL_MODE_WINDING));

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
    RectangleList<int> rectangles{ r };    
    auto size = pimpl->renderingTarget->GetPixelSize();
    rectangles.addWithoutMerging({ 0, 0, (int)size.width, (int)size.height });

    currentState->pushGeometryClipLayer(pimpl->rectListToPathGeometry(rectangles, currentState->transform, D2D1_FILL_MODE_ALTERNATE));
}

void Direct2DLowLevelGraphicsContext::clipToPath (const Path& path, const AffineTransform& transform)
{
    currentState->pushGeometryClipLayer(pimpl->pathToPathGeometry(path, transform.followedBy(currentState->transform)));
}

void Direct2DLowLevelGraphicsContext::clipToImageAlpha (const Image& sourceImage, const AffineTransform& transform)
{
    auto maskImage = sourceImage.convertedToFormat(Image::ARGB);

    ComSmartPtr<ID2D1Bitmap> bitmap;
    ComSmartPtr<ID2D1BitmapBrush> brush;

    D2D1_BRUSH_PROPERTIES brushProps = { 1, transformToMatrix(transform.followedBy(currentState->transform)) };
    auto bmProps = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP);
    auto bp = D2D1::BitmapProperties();

    Image::BitmapData bd{ maskImage, Image::BitmapData::readOnly };
    bp.pixelFormat = pimpl->renderingTarget->GetPixelFormat();
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
 
    auto hr = pimpl->renderingTarget->CreateBitmap(D2D1_SIZE_U{ (UINT32)maskImage.getWidth(), (UINT32)maskImage.getHeight() }, bd.data, bd.lineStride, bp, bitmap.resetAndGetPointerAddress());
    hr = pimpl->renderingTarget->CreateBitmapBrush(bitmap, bmProps, brushProps, brush.resetAndGetPointerAddress());

    auto layerParams = D2D1::LayerParameters();
    layerParams.opacityBrush = brush;

    currentState->pushLayer(layerParams);
}

bool Direct2DLowLevelGraphicsContext::clipRegionIntersects (const Rectangle<int>& r)
{
    return getClipBounds().intersects(r);
}

Rectangle<int> Direct2DLowLevelGraphicsContext::getClipBounds() const
{
    return currentState->clipRect;
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
    // The solid color brush is shared between states, so restore the previous solid color
    //
    if (currentState->fillType.isColour())
    {
        currentState->updateColourBrush();
    }
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

void Direct2DLowLevelGraphicsContext::setInterpolationQuality (Graphics::ResamplingQuality /*quality*/)
{
    //
    // Need a Direct2D 1.1 device context to implement this
    //
}

void Direct2DLowLevelGraphicsContext::fillRect (const Rectangle<int>& r, bool /*replaceExistingContents*/)
{
    fillRect (r.toFloat());
}

void Direct2DLowLevelGraphicsContext::fillRect (const Rectangle<float>& r)
{
    pimpl->renderingTarget->SetTransform (transformToMatrix (currentState->transform));
    currentState->createBrush();
    pimpl->renderingTarget->FillRectangle (rectangleToRectF (r), currentState->currentBrush);
    pimpl->renderingTarget->SetTransform (D2D1::IdentityMatrix());
}

void Direct2DLowLevelGraphicsContext::fillRectList (const RectangleList<float>& list)
{
    for (auto& r : list)
        fillRect (r);
}

void Direct2DLowLevelGraphicsContext::fillPath (const Path& p, const AffineTransform& transform)
{
    currentState->createBrush();
    ComSmartPtr<ID2D1Geometry> geometry (pimpl->pathToPathGeometry (p, transform.followedBy (currentState->transform)));

    if (pimpl->renderingTarget != nullptr && geometry != nullptr)
        pimpl->renderingTarget->FillGeometry (geometry, currentState->currentBrush);
}

void Direct2DLowLevelGraphicsContext::drawImage (const Image& image, const AffineTransform& transform)
{
    pimpl->renderingTarget->SetTransform (transformToMatrix (transform.followedBy (currentState->transform)));

    D2D1_SIZE_U size = { (UINT32) image.getWidth(), (UINT32) image.getHeight() };
    auto bp = D2D1::BitmapProperties();

    Image img (image.convertedToFormat (Image::ARGB));
    Image::BitmapData bd (img, Image::BitmapData::readOnly);
    bp.pixelFormat = pimpl->renderingTarget->GetPixelFormat();
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

    {
        ComSmartPtr<ID2D1Bitmap> tempBitmap;
        pimpl->renderingTarget->CreateBitmap (size, bd.data, bd.lineStride, bp, tempBitmap.resetAndGetPointerAddress());
        if (tempBitmap != nullptr)
            pimpl->renderingTarget->DrawBitmap (tempBitmap);
    }

    pimpl->renderingTarget->SetTransform (D2D1::IdentityMatrix());
}

void Direct2DLowLevelGraphicsContext::drawLine (const Line<float>& line)
{
    pimpl->renderingTarget->SetTransform (transformToMatrix (currentState->transform));
    currentState->createBrush();

    pimpl->renderingTarget->DrawLine (D2D1::Point2F (line.getStartX(), line.getStartY()),
                                      D2D1::Point2F (line.getEndX(), line.getEndY()),
                                      currentState->currentBrush);
    pimpl->renderingTarget->SetTransform (D2D1::IdentityMatrix());
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

    auto hScale = currentState->font.getHorizontalScale();

    pimpl->renderingTarget->SetTransform (transformToMatrix (AffineTransform::scale (hScale, 1.0f)
                                                                             .followedBy (transform)
                                                                             .followedBy (currentState->transform)));

    const auto glyphIndices = (UINT16) glyphNumber;
    const auto glyphAdvances = 0.0f;
    DWRITE_GLYPH_OFFSET offset = { 0.0f, 0.0f };

    DWRITE_GLYPH_RUN glyphRun;
    glyphRun.fontFace = currentState->currentFontFace;
    glyphRun.fontEmSize = (FLOAT) (currentState->font.getHeight() * currentState->fontHeightToEmSizeFactor);
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &glyphIndices;
    glyphRun.glyphAdvances = &glyphAdvances;
    glyphRun.glyphOffsets = &offset;
    glyphRun.isSideways = FALSE;
    glyphRun.bidiLevel = 0;

    pimpl->renderingTarget->DrawGlyphRun ({}, &glyphRun, currentState->currentBrush);
    pimpl->renderingTarget->SetTransform (D2D1::IdentityMatrix());
}

bool Direct2DLowLevelGraphicsContext::drawTextLayout (const AttributedString& text, const Rectangle<float>& area)
{
    pimpl->renderingTarget->SetTransform (transformToMatrix (currentState->transform));

    DirectWriteTypeLayout::drawToD2DContext (text, area,
                                             *(pimpl->renderingTarget),
                                             *(pimpl->factories->directWriteFactory),
                                             *(pimpl->factories->systemFonts));

    pimpl->renderingTarget->SetTransform (D2D1::IdentityMatrix());
    return true;
}

} // namespace juce
