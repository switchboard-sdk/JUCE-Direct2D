
namespace juce
{

    namespace Direct2D
    {

        class RenderTarget
        {
        public:
            RenderTarget(ComSmartPtr< ID2D1Factory1> d2dDedicatedFactory_,
                HWND windowHandle_, 
                DXGI_SWAP_EFFECT swapEffect_, 
                UINT bufferCount_, 
                DXGI_SCALING dxgiScaling_, 
                bool tearingSupported) :
                windowHandle(windowHandle_),
                swapEffect(swapEffect_),
                bufferCount(bufferCount_),
                dxgiScaling(dxgiScaling_),
                swapChainFlags(tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                presentSyncInterval(tearingSupported ? 0 : 1),
                presentFlags(tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0),
                d2dDedicatedFactory(d2dDedicatedFactory_)
            {
                createDeviceContext();
            }

            ~RenderTarget()
            {
            }

            void setScaleFactor(double scaleFactor_)
            {
                dpiScalingFactor = scaleFactor_;

                updateDeviceContextDPI();
                resized();
            }

            double getScaleFactor() const
            {
                return dpiScalingFactor;
            }

            void resized()
            {
                //
                // Get the width & height from the client area; make sure width and height are between 1 and 16384
                //
                int constexpr minSize = 1;
                int constexpr maxSize = 16384;
                auto windowRect = getClientRect().getUnion({ minSize, minSize }).getIntersection({ maxSize, maxSize });
                if (bufferBounds == windowRect)
                {
                    return;
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
                    partialRepaintReady = false;

                    if (SUCCEEDED(hr))
                    {
                        createSwapChainBuffer();
                    }
                    else
                    {
                        releaseDeviceContext();
                    }
                }
            }

            bool canPartiallyRepaint(Rectangle<int> partialRepaintArea)
            {
                return partialRepaintReady && getClientRect().contains(partialRepaintArea);
            }

            void startRender()
            {
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
            }

            void finishRender(Rectangle<int>* updateRect)
            {
                ignoreUnused(updateRect);

                if (deviceContext != nullptr && swapChain != nullptr)
                {
                    auto hr = deviceContext->EndDraw();

                    deviceContext->SetTarget(nullptr);

                    if (SUCCEEDED(hr))
                    {
#if JUCE_DIRECT2D_PARTIAL_REPAINT
                        if (updateRect != nullptr && !updateRect->isEmpty() && bufferBounds.contains(*updateRect))
                        {
                            RECT dirtyRectangle
                            {
                                updateRect->getX(),
                                updateRect->getY(),
                                updateRect->getRight(),
                                updateRect->getBottom()
                            };
                            DXGI_PRESENT_PARAMETERS presentParameters
                            {
                                1,
                                &dirtyRectangle,
                                nullptr,
                                nullptr
                            };

                            jassert(partialRepaintReady);

                            hr = swapChain->Present1(presentSyncInterval, presentFlags, &presentParameters);
                            if (SUCCEEDED(hr))
                            {
                                ValidateRect(windowHandle, &dirtyRectangle);
                            }
                            else if (DXGI_ERROR_INVALID_CALL == hr)
                            {
                                hr = swapChain->Present(presentSyncInterval, presentFlags);
                                ValidateRect(windowHandle, nullptr);
                            }
                            else
                            {
                                jassertfalse;
                            }
                        }
                        else
#endif
                        {
                            hr = swapChain->Present(presentSyncInterval, presentFlags);
                            partialRepaintReady = true;

                            ValidateRect(windowHandle, nullptr);
                        }
                    }

                    if (S_OK != hr && DXGI_STATUS_OCCLUDED != hr)
                    {
                        releaseDeviceContext();
                    }
                }
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
                GetClientRect(windowHandle, &windowRect);
                
                return juce::Rectangle<int>::leftTopRightBottom(windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
            }

        private:
            HWND const windowHandle;
            DXGI_SWAP_EFFECT const swapEffect;
            UINT const bufferCount;
            DXGI_SCALING const dxgiScaling;
            double dpiScalingFactor = 1.0;
            juce::Rectangle<int> bufferBounds{ 1, 1 };
            uint32 const swapChainFlags;
            uint32 const presentSyncInterval;
            uint32 const presentFlags;
            bool partialRepaintReady = false;
            ComSmartPtr< ID2D1Factory1> d2dDedicatedFactory;
            ComSmartPtr<ID2D1DeviceContext> deviceContext;
            ComSmartPtr<IDXGISwapChain1> swapChain;
            ComSmartPtr<ID2D1Bitmap1> swapChainBuffer;
            ComSmartPtr<ID2D1SolidColorBrush> colourBrush;
            ComSmartPtr<IDCompositionDevice> compositionDevice;
            ComSmartPtr<IDCompositionTarget> compositionTarget;
            ComSmartPtr<IDCompositionVisual> compositionVisual;

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

                                        partialRepaintReady = false;

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
                                                hr = compositionDevice->CreateTargetForHwnd(windowHandle, FALSE, compositionTarget.resetAndGetPointerAddress());
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
                partialRepaintReady = false;
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
        };

    } // namespace Direct2D

}
