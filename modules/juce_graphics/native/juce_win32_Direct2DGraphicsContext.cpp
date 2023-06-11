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

    namespace Direct2D
    {
        template <typename Type>
        D2D1_RECT_F rectangleToRectF(const Rectangle<Type>& r)
        {
            return { (float)r.getX(), (float)r.getY(), (float)r.getRight(), (float)r.getBottom() };
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

    } // namespace Direct2D

//==============================================================================

struct Direct2DLowLevelGraphicsContext::Pimpl
{
    Pimpl(HWND hwnd_, bool tearingSupported_) :
        hwnd(hwnd_)
    {
#if JUCE_DEBUG
        D2D1_FACTORY_OPTIONS options{ D2D1_DEBUG_LEVEL_INFORMATION };
#else
        D2D1_FACTORY_OPTIONS options{ D2D1_DEBUG_LEVEL_NONE };
#endif

        auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(d2dDedicatedFactory.resetAndGetPointerAddress()));
        jassertquiet(SUCCEEDED(hr));

#if JUCE_DIRECT2D_PARTIAL_REPAINT
        renderTarget = std::make_unique<Direct2D::RenderTarget>(d2dDedicatedFactory,
            hwnd_, 
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 
            2, 
            DXGI_SCALING_STRETCH, 
            tearingSupported_);
#else
        renderTarget = std::make_unique<Direct2D::RenderTarget>(d2dDedicatedFactory,
            hwnd_, 
            DXGI_SWAP_EFFECT_FLIP_DISCARD, 
            2, 
            DXGI_SCALING_STRETCH, 
            tearingSupported_);
#endif
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
            Direct2D::rectToGeometrySink(rect, objects.sink, transform);
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
                Direct2D::rectToGeometrySink(clipRegion.getRectangle(i), objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ComSmartPtr<ID2D1Geometry> pathToPathGeometry (const Path& path, const AffineTransform& transform)
    {
        ScopedGeometryWithSink objects{ d2dDedicatedFactory, path.isUsingNonZeroWinding() ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE };

        if (objects.sink != nullptr)
        {
            Direct2D::pathToGeometrySink(path, objects.sink, transform);

            return { (ID2D1Geometry*)objects.geometry };
        }

        return nullptr;
    }

    ID2D1DeviceContext* const getDeviceContext() const 
    {
        return renderTarget->getDeviceContext();
    }

    ID2D1SolidColorBrush* const getColourBrush() const
    {
        return renderTarget->getColourBrush();
    }

    bool resized()
    {
        return renderTarget->resized();
    }

    void startRender()
    {
        renderTarget->startRender();
    }

    void finishRender(Rectangle<int>* updateRect)
    {
        renderTarget->finishRender(updateRect);
    }

    bool canPartiallyRepaint(Rectangle<int> partialRepaintArea) const
    {
        return renderTarget->canPartiallyRepaint(partialRepaintArea);
    }

    void setScaleFactor(double scale_)
    {
        renderTarget->setScaleFactor(scale_);
    }

    double getScaleFactor() const
    {
        return renderTarget->getScaleFactor();
    }

    SharedResourcePointer<Direct2DFactories> sharedFactories;
    ComSmartPtr<ID2D1Factory1> d2dDedicatedFactory;
    ComSmartPtr<ID2D1StrokeStyle> strokeStyle;

private:
    HWND hwnd = nullptr;
    std::unique_ptr<Direct2D::RenderTarget> renderTarget;
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
            deviceContext->PushAxisAlignedClip(Direct2D::rectangleToRectF(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

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
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), Direct2D::transformToMatrix (fillType.transform) };
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
                D2D1_BRUSH_PROPERTIES brushProps = { fillType.getOpacity(), Direct2D::transformToMatrix(fillType.transform) };
                const int numColors = fillType.gradient->getNumColours();

                HeapBlock<D2D1_GRADIENT_STOP> stops (numColors);

                for (int i = fillType.gradient->getNumColours(); --i >= 0;)
                {
                    stops[i].color = Direct2D::colourToD2D (fillType.gradient->getColour (i));
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
            auto colour = Direct2D::colourToD2D(fillType.colour);
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
Direct2DLowLevelGraphicsContext::Direct2DLowLevelGraphicsContext (HWND hwnd_)
    : currentState (nullptr),
      pimpl (new Pimpl(hwnd_, Direct2D::isTearingSupported()))
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

void Direct2DLowLevelGraphicsContext::start()
{
    pimpl->startRender();

    saveState();
}

void Direct2DLowLevelGraphicsContext::end(Rectangle<int>* updateRect)
{
    while (states.size() > 0)
    {
        states.removeLast(1);
    }
    currentState = nullptr;

    pimpl->finishRender(updateRect);
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

    if (Direct2D::isTransformOnlyTranslationOrScale(currentTransform))
    {
        //
        // If the current transform is just a translation, use an axis-aligned clip layer (according to the Direct2D debug layer)
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
    auto const currentTransform = currentState->currentTransform.getTransform();
    auto transformedR = clipRegion.getBounds().transformedBy(currentTransform);
    transformedR.intersectRectangle(currentState->clipRegion);

    currentState->pushGeometryClipLayer(pimpl->rectListToPathGeometry(clipRegion, currentTransform, D2D1_FILL_MODE_WINDING));

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

        D2D1_BRUSH_PROPERTIES brushProps = { 1, Direct2D::transformToMatrix(chainedTransform) };
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
        deviceContext->FillRectangle(Direct2D::rectangleToRectF(r), currentState->currentBrush);
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
        deviceContext->DrawRectangle(Direct2D::rectangleToRectF(r), currentState->currentBrush, lineThickness);
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
            deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
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

            deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransformWith(transform)));

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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(deviceContextTransform));

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
        currentState->currentBrush->SetTransform(Direct2D::transformToMatrix(scaledTransform.inverted()));

        deviceContext->DrawGlyphRun({}, &glyphRun, currentState->currentBrush);

        deviceContext->SetTransform(D2D1::IdentityMatrix());
        currentState->currentBrush->SetTransform(brushTransform);
    }
}

bool Direct2DLowLevelGraphicsContext::drawTextLayout (const AttributedString& text, const Rectangle<float>& area)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));

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
    DBG("setScaleFactor " << scale_);
    pimpl->setScaleFactor(scale_);
}

double Direct2DLowLevelGraphicsContext::getScaleFactor() const
{
    return pimpl->getScaleFactor();
}

bool Direct2DLowLevelGraphicsContext::canPartiallyRepaint(Rectangle<int> partialRepaintArea) const
{
    return pimpl->canPartiallyRepaint(partialRepaintArea);
}

bool Direct2DLowLevelGraphicsContext::drawRoundedRectangle(Rectangle<float> area, float cornerSize, float lineThickness)
{
    if (auto deviceContext = pimpl->getDeviceContext())
    {
        currentState->createBrush();
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));

        D2D1_ROUNDED_RECT roundedRect
        {
            Direct2D::rectangleToRectF(area),
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));
        
        D2D1_ROUNDED_RECT roundedRect
        {
            Direct2D::rectangleToRectF(area),
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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));

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
        deviceContext->SetTransform(Direct2D::transformToMatrix(currentState->currentTransform.getTransform()));

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
        deviceContext->SetTransform(Direct2D::transformToMatrix(deviceContextTransform));

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
        currentState->currentBrush->SetTransform(Direct2D::transformToMatrix(scaledTransform.inverted()));

        deviceContext->DrawGlyphRun({}, &directWriteGlyphRun, currentState->currentBrush);

        deviceContext->SetTransform(D2D1::IdentityMatrix());
        currentState->currentBrush->SetTransform(brushTransform);
    }
}

} // namespace juce
