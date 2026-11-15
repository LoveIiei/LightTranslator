#pragma once
#include <windows.h>
#include <ole2.h> // WIN32_LEAN_AND_MEAN drops the COM headers the graphics headers need
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wil/com.h>

// D3D11 device -> composition swap chain (flip-sequential, premultiplied alpha) ->
// ID2D1DeviceContext drawing into the back buffer -> DirectComposition visual bound to the HWND.
// (ID2D1HwndRenderTarget cannot do per-pixel transparency over a system backdrop.)
// All resources stay alive while the popup is hidden so re-showing is instant.
class PopupRenderer {
public:
    HRESULT Initialize(HWND hwnd);

    IDWriteFactory* DWrite() const { return m_dwrite.get(); }

    // Prepares the back buffer for a widthPx x heightPx frame and calls BeginDraw.
    // Returns nullptr if the device could not be (re)created.
    ID2D1DeviceContext* BeginDraw(UINT widthPx, UINT heightPx, float dpi);

    // Ends the frame and presents it. Returns false when the device was lost; resources have
    // been released and the caller should simply draw again.
    bool EndDraw();

    // A shared solid brush, recolored per use (valid between BeginDraw and EndDraw).
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color);

private:
    HRESULT CreateDeviceResources(UINT widthPx, UINT heightPx);
    HRESULT CreateTargetBitmap(float dpi);
    void ReleaseDeviceResources();

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    float m_dpi = 0;

    // Device-independent.
    wil::com_ptr_nothrow<IDWriteFactory> m_dwrite;
    wil::com_ptr_nothrow<ID2D1Factory1> m_d2dFactory;

    // Device-dependent (rebuilt on D2DERR_RECREATE_TARGET / DXGI_ERROR_DEVICE_REMOVED).
    wil::com_ptr_nothrow<ID3D11Device> m_d3dDevice;
    wil::com_ptr_nothrow<IDXGISwapChain1> m_swapChain;
    wil::com_ptr_nothrow<ID2D1Device> m_d2dDevice;
    wil::com_ptr_nothrow<ID2D1DeviceContext> m_context;
    wil::com_ptr_nothrow<ID2D1Bitmap1> m_target;
    wil::com_ptr_nothrow<ID2D1SolidColorBrush> m_brush;
    wil::com_ptr_nothrow<IDCompositionDevice> m_dcompDevice;
    wil::com_ptr_nothrow<IDCompositionTarget> m_dcompTarget;
    wil::com_ptr_nothrow<IDCompositionVisual> m_visual;
};
