
namespace juce
{

    namespace Direct2D
    {

        class ChildWindow
        {
        public:
            ChildWindow(String className_, HWND parentHwnd_, DXGI_SWAP_EFFECT swapEffect_, UINT bufferCount_, DXGI_SCALING scaling_, bool tearingSupported) :
                parentHwnd(parentHwnd_),
                swapEffect(swapEffect_),
                bufferCount(bufferCount_),
                scaling(scaling_),
                swapChainFlags(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING),
                presentSyncInterval(0),
                presentFlags(DXGI_PRESENT_ALLOW_TEARING)

//                 swapChainFlags(tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
//                 presentSyncInterval(tearingSupported ? 0 : 1),
//                 presentFlags(tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0)
            {
                HMODULE moduleHandle = (HMODULE)Process::getCurrentModuleInstanceHandle();

                RECT parentRect;
                GetClientRect(parentHwnd_, &parentRect);

                hwnd = CreateWindowW(className_.toWideCharPointer(),
                    nullptr,
                    WS_CHILD | WS_DISABLED, // Specify WS_DISABLED to pass input events to parent window
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    parentRect.right - parentRect.left,
                    parentRect.bottom - parentRect.top,
                    parentHwnd_,
                    nullptr,
                    moduleHandle,
                    this
                );
                if (hwnd)
                {
                    setVisible(true);
                }
            }

            ~ChildWindow()
            {
                DestroyWindow(hwnd);
            }

            void resized()
            {
                //
                // Get the size of the parent window
                //
                RECT windowRect;
                GetClientRect(parentHwnd, &windowRect);
                auto width = windowRect.right - windowRect.left;
                auto height = windowRect.bottom - windowRect.top;

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

                    auto hr = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
                    if (SUCCEEDED(hr))
                    {
                        createSwapChainBuffer();
                    }
                    else
                    {
                        releaseDeviceContext();
                    }
                }

                //
                // Resize this child window
                //
                MoveWindow(hwnd, 0, 0, width, height, TRUE);
            }

            bool needsFullRender() const
            {
                return !allBuffersPresented;
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
                if (deviceContext != nullptr && swapChain != nullptr)
                {
                    auto hr = deviceContext->EndDraw();

                    deviceContext->SetTarget(nullptr);

                    if (SUCCEEDED(hr))
                    {
                        if (updateRect != nullptr && allBuffersPresented)
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
                        }
                        else
                        {
                            hr = swapChain->Present(presentSyncInterval, presentFlags);

                            //
                            // Every buffer in the swap chain needs to be presented without dirty rectangles once
                            // before calling Present1 with dirty rectangles
                            // 
                            if (!allBuffersPresented)
                            {
                                ++numPresentedBuffers;
                                allBuffersPresented = numPresentedBuffers >= bufferCount;
                            }
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

            void setVisible(bool visible)
            {
                ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
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
            DXGI_SCALING const scaling;
            uint32 const swapChainFlags;
            uint32 const presentSyncInterval;
            uint32 const presentFlags;
            bool allBuffersPresented = false;
            UINT numPresentedBuffers = 0;
            HWND hwnd = nullptr;
            SharedResourcePointer<Direct2DFactories> factories;
            ComSmartPtr<ID2D1DeviceContext> deviceContext;
            ComSmartPtr<IDXGISwapChain1> swapChain;
            ComSmartPtr<ID2D1Bitmap1> swapChainBuffer;
            ComSmartPtr<ID2D1SolidColorBrush> colourBrush;

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
                                        swapChainDescription.SampleDesc.Count = 1;
                                        swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                        swapChainDescription.BufferCount = bufferCount;
                                        swapChainDescription.SwapEffect = swapEffect;
                                        swapChainDescription.Scaling = scaling;
                                        swapChainDescription.Flags = swapChainFlags;
                                        hr = dxgiFactory->CreateSwapChainForHwnd(direct3DDevice,
                                            hwnd,
                                            &swapChainDescription,
                                            nullptr,
                                            nullptr,
                                            swapChain.resetAndGetPointerAddress());
                                        if (SUCCEEDED(hr))
                                        {
                                            allBuffersPresented = false;
                                            numPresentedBuffers = 0;

                                            ComSmartPtr<ID2D1Device> direct2DDevice;
                                            hr = factories->d2dFactory->CreateDevice(dxgiDevice, direct2DDevice.resetAndGetPointerAddress());
                                            if (SUCCEEDED(hr))
                                            {
                                                hr = direct2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, deviceContext.resetAndGetPointerAddress());
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

                allBuffersPresented = false;
                numPresentedBuffers = 0;
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

        };

    } // namespace Direct2D

}
