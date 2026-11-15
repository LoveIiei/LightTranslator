#include "ui/PopupRenderer.h"

#include <algorithm>

HRESULT PopupRenderer::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;
    RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(m_dwrite.put())));
    D2D1_FACTORY_OPTIONS options{};
    RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                       reinterpret_cast<void**>(m_d2dFactory.put())));
    return S_OK;
}

HRESULT PopupRenderer::CreateDeviceResources(UINT widthPx, UINT heightPx)
{
    ReleaseDeviceResources();

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                   m_d3dDevice.put(), nullptr, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               m_d3dDevice.put(), nullptr, nullptr);
    }
    RETURN_IF_FAILED(hr);

    wil::com_ptr_nothrow<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(m_d3dDevice.query_to(&dxgiDevice));
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(&adapter));
    wil::com_ptr_nothrow<IDXGIFactory2> factory;
    RETURN_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max(widthPx, 1u);
    desc.Height = std::max(heightPx, 1u);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    RETURN_IF_FAILED(factory->CreateSwapChainForComposition(m_d3dDevice.get(), &desc, nullptr, m_swapChain.put()));
    m_width = desc.Width;
    m_height = desc.Height;

    RETURN_IF_FAILED(m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
    RETURN_IF_FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_context.put()));
    RETURN_IF_FAILED(m_context->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), m_brush.put()));

    RETURN_IF_FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(&m_dcompDevice)));
    RETURN_IF_FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.put()));
    RETURN_IF_FAILED(m_dcompDevice->CreateVisual(m_visual.put()));
    RETURN_IF_FAILED(m_visual->SetContent(m_swapChain.get()));
    RETURN_IF_FAILED(m_dcompTarget->SetRoot(m_visual.get()));
    RETURN_IF_FAILED(m_dcompDevice->Commit());
    return S_OK;
}

HRESULT PopupRenderer::CreateTargetBitmap(float dpi)
{
    wil::com_ptr_nothrow<IDXGISurface> surface;
    RETURN_IF_FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)));
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    RETURN_IF_FAILED(m_context->CreateBitmapFromDxgiSurface(surface.get(), &props, m_target.put()));
    m_context->SetTarget(m_target.get());
    m_dpi = dpi;
    return S_OK;
}

void PopupRenderer::ReleaseDeviceResources()
{
    if (m_context) {
        m_context->SetTarget(nullptr);
    }
    m_target.reset();
    m_brush.reset();
    m_visual.reset();
    m_dcompTarget.reset();
    m_dcompDevice.reset();
    m_context.reset();
    m_d2dDevice.reset();
    m_swapChain.reset();
    m_d3dDevice.reset();
    m_width = m_height = 0;
    m_dpi = 0;
}

ID2D1DeviceContext* PopupRenderer::BeginDraw(UINT widthPx, UINT heightPx, float dpi)
{
    widthPx = std::max(widthPx, 1u);
    heightPx = std::max(heightPx, 1u);

    if (!m_context && FAILED(CreateDeviceResources(widthPx, heightPx))) {
        ReleaseDeviceResources();
        return nullptr;
    }
    if (widthPx != m_width || heightPx != m_height) {
        // The back buffer must not be referenced while the swap chain is resized.
        m_context->SetTarget(nullptr);
        m_target.reset();
        if (FAILED(m_swapChain->ResizeBuffers(0, widthPx, heightPx, DXGI_FORMAT_UNKNOWN, 0))) {
            if (FAILED(CreateDeviceResources(widthPx, heightPx))) {
                ReleaseDeviceResources();
                return nullptr;
            }
        }
        m_width = widthPx;
        m_height = heightPx;
    }
    if (m_target && m_dpi != dpi) {
        m_context->SetTarget(nullptr);
        m_target.reset();
    }
    if (!m_target && FAILED(CreateTargetBitmap(dpi))) {
        ReleaseDeviceResources();
        return nullptr;
    }
    m_context->SetDpi(dpi, dpi);
    // ClearType needs an opaque background; we draw over a transparent backdrop.
    m_context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_context->BeginDraw();
    return m_context.get();
}

bool PopupRenderer::EndDraw()
{
    if (!m_context) {
        return true;
    }
    HRESULT hr = m_context->EndDraw();
    if (SUCCEEDED(hr)) {
        hr = m_swapChain->Present(1, 0);
    }
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ReleaseDeviceResources(); // rebuilt on the next BeginDraw
        return false;
    }
    return true;
}

ID2D1SolidColorBrush* PopupRenderer::Brush(const D2D1_COLOR_F& color)
{
    if (m_brush) {
        m_brush->SetColor(color);
    }
    return m_brush.get();
}
