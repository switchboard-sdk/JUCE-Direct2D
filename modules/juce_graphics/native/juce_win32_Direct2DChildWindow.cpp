
namespace juce
{

    namespace Direct2D
    {

        class ChildWindow
        {
        public:
            ChildWindow(String className_, HWND parentHwnd_, DXGI_SWAP_EFFECT swapEffect_, UINT bufferCount_, DXGI_SCALING dxgiScaling_, bool tearingSupported, double scaleFactor_) :
                parentHwnd(parentHwnd_),
                swapEffect(swapEffect_),
                bufferCount(bufferCount_),
                dxgiScaling(dxgiScaling_),
                scaleFactor(scaleFactor_),
                swapChainFlags(tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                presentSyncInterval(tearingSupported ? 0 : 1),
                presentFlags(tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0)
            {
                HMODULE moduleHandle = (HMODULE)Process::getCurrentModuleInstanceHandle();

                RECT parentRect;
                GetClientRect(parentHwnd_, &parentRect);

                int width = roundToInt((parentRect.right - parentRect.left) * scaleFactor_);
                int height = roundToInt((parentRect.bottom - parentRect.top) * scaleFactor_);

                hwnd = CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP,
                    className_.toWideCharPointer(),
                    nullptr,
                    WS_VISIBLE | WS_CHILD | WS_DISABLED, // Specify WS_DISABLED to pass input events to parent window
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    width,
                    height,
                    parentHwnd_,
                    nullptr,
                    moduleHandle,
                    this
                );

                if (hwnd)
                {
                    createDeviceContext();
                }
            }

            ~ChildWindow()
            {
                DestroyWindow(hwnd);
            }

            void setScaleFactor(double scaleFactor_)
            {
                scaleFactor = scaleFactor_;

                updateDeviceContextDPI();
                resized();
            }

            double getScaleFactor() const
            {
                return scaleFactor;
            }

            void resized()
            {
                //
                // Get the size of the parent window
                //
                RECT windowRect;
                GetClientRect(parentHwnd, &windowRect);

                //
                // Resize this child window
                //
                auto width = windowRect.right - windowRect.left;
                auto height = windowRect.bottom - windowRect.top;
                width = juce::jmax(width, 1l);
                height = juce::jmax(height, 1l);
                MoveWindow(hwnd, 0, 0, width, height, FALSE /* repaint */);

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

                    auto scaledWidth = roundToInt(width * scaleFactor);
                    auto scaledHeight = roundToInt(height * scaleFactor);
                    auto hr = swapChain->ResizeBuffers(0, scaledWidth, scaledHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags);

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
                        if (updateRect != nullptr && !updateRect->isEmpty())
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

                            hr = swapChain->Present1(presentSyncInterval, presentFlags, &presentParameters);
                            if (SUCCEEDED(hr))
                            {
                                ValidateRect(hwnd, &dirtyRectangle);
                            }
                            else if (DXGI_ERROR_INVALID_CALL == hr)
                            {
                                hr = swapChain->Present(presentSyncInterval, presentFlags);
                                ValidateRect(hwnd, nullptr);
                            }
                        }
                        else
#endif
                        {
                            hr = swapChain->Present(presentSyncInterval, presentFlags);
                            ValidateRect(hwnd, nullptr);
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

            struct Class
            {
                Class()
                {
                    HMODULE moduleHandle = (HMODULE)Process::getCurrentModuleInstanceHandle();
                    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
                    wcex.style = CS_HREDRAW | CS_VREDRAW;
                    wcex.lpfnWndProc = windowProc;
                    wcex.cbClsExtra = 0;
                    wcex.cbWndExtra = sizeof(LONG_PTR);
                    wcex.hInstance = moduleHandle;
                    wcex.hbrBackground = nullptr;
                    wcex.lpszMenuName = nullptr;
                    wcex.lpszClassName = className.toWideCharPointer();
                    RegisterClassExW(&wcex);
                }

                ~Class()
                {
                    HMODULE moduleHandle = (HMODULE)Process::getCurrentModuleInstanceHandle();
                    UnregisterClassW(className.toWideCharPointer(), moduleHandle);
                }

                String const className{ "JUCE_Direct2D_" + String::toHexString(Time::getHighResolutionTicks()) };
            };

        private:
            HWND const parentHwnd;
            DXGI_SWAP_EFFECT const swapEffect;
            UINT const bufferCount;
            DXGI_SCALING const dxgiScaling;
            double scaleFactor;
            uint32 const swapChainFlags;
            uint32 const presentSyncInterval;
            uint32 const presentFlags;
            HWND hwnd = nullptr;
            SharedResourcePointer<Direct2DFactories> factories;
            ComSmartPtr<ID2D1DeviceContext> deviceContext;
            ComSmartPtr<IDXGISwapChain1> swapChain;
            ComSmartPtr<ID2D1Bitmap1> swapChainBuffer;
            ComSmartPtr<ID2D1SolidColorBrush> colourBrush;

#if JUCE_DIRECT2D_USE_DIRECT_COMPOSITION
            juce::ComSmartPtr<IDCompositionDevice> compositionDevice;
            juce::ComSmartPtr<IDCompositionTarget> compositionTarget;
            juce::ComSmartPtr<IDCompositionVisual> compositionVisual;
#endif

            static LRESULT CALLBACK windowProc
            (
                HWND hwnd,
                UINT message,
                WPARAM wParam,
                LPARAM lParam
            )
            {
                switch (message)
                {
                case WM_CREATE:
                    return 0;

                case WM_ERASEBKGND:
                    return 1;

                case WM_PAINT:
                    ValidateRect(hwnd, nullptr);
                    return 0;
                }

                return DefWindowProcW(hwnd, message, wParam, lParam);
            }

            void createDeviceContext()
            {
                if (factories->d2dFactory != nullptr)
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
                                        swapChainDescription.Width = 1;
                                        swapChainDescription.Height = 1;
                                        swapChainDescription.SampleDesc.Count = 1;
                                        swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                        swapChainDescription.BufferCount = bufferCount;
                                        swapChainDescription.SwapEffect = swapEffect;
                                        swapChainDescription.Scaling = dxgiScaling;
                                        swapChainDescription.Flags = swapChainFlags;

#if JUCE_DIRECT2D_USE_DIRECT_COMPOSITION
                                        hr = dxgiFactory->CreateSwapChainForComposition(direct3DDevice,
                                            &swapChainDescription,
                                            nullptr,
                                            swapChain.resetAndGetPointerAddress());
#else
                                        hr = dxgiFactory->CreateSwapChainForHwnd(direct3DDevice,
                                            hwnd,
                                            &swapChainDescription,
                                            nullptr,
                                            nullptr,
                                            swapChain.resetAndGetPointerAddress());
#endif

                                        if (SUCCEEDED(hr))
                                        {
                                            ComSmartPtr<ID2D1Device> direct2DDevice;
                                            hr = factories->d2dFactory->CreateDevice(dxgiDevice, direct2DDevice.resetAndGetPointerAddress());
                                            if (SUCCEEDED(hr))
                                            {
                                                hr = direct2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, deviceContext.resetAndGetPointerAddress());
                                                if (SUCCEEDED(hr))
                                                {
                                                    updateDeviceContextDPI();
                                                }
                                            }
                                        }

#if JUCE_DIRECT2D_USE_DIRECT_COMPOSITION
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
#endif
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
                    float scaledDPI = windowsDefaultDPI * (float)scaleFactor;
                    deviceContext->SetDpi(scaledDPI, scaledDPI);
                }
            }
        };

    } // namespace Direct2D

}
