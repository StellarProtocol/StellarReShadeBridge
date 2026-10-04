// Stellar ReShade bridge — the swap chain object handed to the isolated effect runtime.
//
// ReShade's add-on API creates an extra effect runtime only around an IDXGISwapChain (ReShadeCreateEffectRuntime,
// source/addon.cpp:170-192 in ReShade 6.8.0: it QueryInterfaces the object for IDXGISwapChain and requires GetDevice to
// return the very device passed in). A REAL swap chain cannot be used here: in a game ReShade hooks
// IDXGIFactory::CreateSwapChain (source/dxgi/dxgi.cpp:400-438), finds the game's device behind any device pointer
// (query_device falls back to the device's private data, dxgi.cpp:316-340), and wraps the new swap chain in its own
// proxy, which creates a second AUTOMATIC effect runtime with the game's ReShade.ini (dxgi_swapchain.cpp:63-77,
// runtime_manager.cpp:15-46). A hidden window would also need a presentable surface under every D3D11 implementation.
//
// So the bridge implements the interface itself, backed by one texture it owns. ReShade's D3D11 swap chain wrapper only
// calls GetDesc (window handle, buffer format), GetBuffer(0), GetPrivateData (a colour-space GUID) and QueryInterface for
// IDXGISwapChain3 (source/d3d11/d3d11_impl_swapchain.cpp:10-79); get_back_buffer_count is 1 and the current index 0
// (d3d11_impl_swapchain.hpp:23-24). Add-on private data on the runtime lives inside ReShade's own wrapper objects
// (source/reshade_api_object_impl.hpp), never on this COM object. Nothing presents it: Present is a no-op.
//
// OutputWindow is null on purpose: the runtime then registers no input window (runtime.cpp:556-562), so no hotkey,
// mouse or keyboard state of the game reaches it.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>

namespace stellar_rsb
{
    class IsolatedSwapChain final : public IDXGISwapChain
    {
    public:
        // Takes its own references to `device` and `buffer`.
        IsolatedSwapChain(ID3D11Device *device, ID3D11Texture2D *buffer, UINT width, UINT height) :
            _device(device), _buffer(buffer), _width(width), _height(height)
        {
            _device->AddRef();
            _buffer->AddRef();
        }

        IsolatedSwapChain(const IsolatedSwapChain &) = delete;
        IsolatedSwapChain &operator=(const IsolatedSwapChain &) = delete;

        // IUnknown
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
        {
            if (object == nullptr)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
                riid == __uuidof(IDXGISwapChain))
            {
                AddRef();
                *object = static_cast<IDXGISwapChain *>(this);
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++_refs); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const long refs = --_refs;
            if (refs == 0)
                delete this;
            return static_cast<ULONG>(refs);
        }

        // IDXGIObject
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *size, void *) override
        {
            if (size != nullptr)
                *size = 0;
            return DXGI_ERROR_NOT_FOUND; // ReShade then assumes an sRGB colour space for an 8-bit buffer
        }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **parent) override
        {
            if (parent != nullptr)
                *parent = nullptr;
            return E_NOINTERFACE;
        }

        // IDXGIDeviceSubObject
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
        {
            if (device == nullptr)
                return E_POINTER;
            return _device->QueryInterface(riid, device);
        }

        // IDXGISwapChain
        HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void **surface) override
        {
            if (surface == nullptr)
                return E_POINTER;
            if (index != 0)
            {
                *surface = nullptr;
                return DXGI_ERROR_INVALID_CALL;
            }
            return _buffer->QueryInterface(riid, surface);
        }
        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput *) override { return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; }
        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) override
        {
            if (fullscreen != nullptr)
                *fullscreen = FALSE;
            if (target != nullptr)
                *target = nullptr;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override
        {
            if (desc == nullptr)
                return E_POINTER;
            *desc = {};
            desc->BufferDesc.Width = _width;
            desc->BufferDesc.Height = _height;
            desc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc->SampleDesc.Count = 1;
            desc->BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
            desc->BufferCount = 1;
            desc->OutputWindow = nullptr;
            desc->Windowed = TRUE;
            desc->SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *) override { return DXGI_ERROR_INVALID_CALL; }
        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override
        {
            if (output != nullptr)
                *output = nullptr;
            return DXGI_ERROR_UNSUPPORTED;
        }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *) override { return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override
        {
            if (count == nullptr)
                return E_POINTER;
            *count = 0;
            return S_OK;
        }

    private:
        ~IsolatedSwapChain()
        {
            _buffer->Release();
            _device->Release();
        }

        std::atomic<long> _refs{ 1 };
        ID3D11Device *_device;
        ID3D11Texture2D *_buffer;
        UINT _width;
        UINT _height;
    };
}
