#include "runtime.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr GLenum WGL_ACCESS_READ_ONLY_NV = 0x00000000;
constexpr GLenum WGL_ACCESS_READ_WRITE_NV = 0x00000001;
constexpr GLenum WGL_ACCESS_WRITE_DISCARD_NV = 0x00000002;
constexpr GLenum GL_TEXTURE_BINDING_2D_VALUE = 0x8069;
constexpr GLenum GL_TEXTURE_MAX_LEVEL_VALUE = 0x813D;
constexpr GLenum GL_TEXTURE_INTERNAL_FORMAT_VALUE = 0x1003;
constexpr GLenum GL_TEXTURE_WIDTH_VALUE = 0x1000;
constexpr GLenum GL_TEXTURE_HEIGHT_VALUE = 0x1001;
constexpr GLenum GL_RGBA8_VALUE = 0x8058;
constexpr GLenum GL_SRGB_ALPHA_VALUE = 0x8C42;
constexpr GLenum GL_SRGB8_ALPHA8_VALUE = 0x8C43;
constexpr GLenum GL_READ_FRAMEBUFFER_VALUE = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_VALUE = 0x8CA9;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING_VALUE = 0x8CA6;
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING_VALUE = 0x8CAA;
constexpr GLenum GL_COLOR_ATTACHMENT0_VALUE = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_VALUE = 0x8CD5;
constexpr GLenum GL_FRAMEBUFFER_SRGB_VALUE = 0x8DB9;
constexpr DWORD INTEROP_WAIT_MS = 5000;

using DXSetShareFn = BOOL(WINAPI *)(void *, HANDLE);
using DXOpenDeviceFn = HANDLE(WINAPI *)(void *);
using DXCloseDeviceFn = BOOL(WINAPI *)(HANDLE);
using DXRegisterObjectFn = HANDLE(WINAPI *)(HANDLE, void *, GLuint, GLenum, GLenum);
using DXUnregisterObjectFn = BOOL(WINAPI *)(HANDLE, HANDLE);
using DXObjectAccessFn = BOOL(WINAPI *)(HANDLE, GLenum);
using DXLockObjectsFn = BOOL(WINAPI *)(HANDLE, GLint, HANDLE *);
using DXUnlockObjectsFn = BOOL(WINAPI *)(HANDLE, GLint, HANDLE *);
using WGLExtensionsARBFn = const char *(WINAPI *)(HDC);
using WGLExtensionsEXTFn = const char *(WINAPI *)(void);
using GLBindTextureFn = void(APIENTRY *)(GLenum, GLuint);
using GLGetStringFn = const GLubyte *(APIENTRY *)(GLenum);
using GLGetIntegervFn = void(APIENTRY *)(GLenum, GLint *);
using GLTexParameteriFn = void(APIENTRY *)(GLenum, GLenum, GLint);
using GLGetTexLevelParameterivFn = void(APIENTRY *)(GLenum, GLint, GLenum, GLint *);
using GLIsTextureFn = GLboolean(APIENTRY *)(GLuint);
using GLGetErrorFn = GLenum(APIENTRY *)(void);
using GLFinishFn = void(APIENTRY *)(void);
using GLIsEnabledFn = GLboolean(APIENTRY *)(GLenum);
using GLEnableFn = void(APIENTRY *)(GLenum);
using GLDisableFn = void(APIENTRY *)(GLenum);
using GLGenFramebuffersFn = void(APIENTRY *)(GLsizei, GLuint *);
using GLDeleteFramebuffersFn = void(APIENTRY *)(GLsizei, const GLuint *);
using GLBindFramebufferFn = void(APIENTRY *)(GLenum, GLuint);
using GLFramebufferTexture2DFn = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using GLCheckFramebufferStatusFn = GLenum(APIENTRY *)(GLenum);
using GLBlitFramebufferFn = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using GLCopyImageSubDataFn = void(APIENTRY *)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint,
                                              GLsizei, GLsizei, GLsizei);

template <typename T>
T NativeProc(const char *name) {
    return reinterpret_cast<T>(IntelOpenGLVRFix::SystemWGLProc(name));
}

template <typename T>
T CoreProc(const char *name) {
    return reinterpret_cast<T>(IntelOpenGLVRFix::SystemExport(name));
}

struct GLAPI {
    GLBindTextureFn bindTexture = nullptr;
    GLGetIntegervFn getInteger = nullptr;
    GLTexParameteriFn texParameter = nullptr;
    GLGetErrorFn getError = nullptr;
    GLFinishFn finish = nullptr;

    bool Load() {
        bindTexture = CoreProc<GLBindTextureFn>("glBindTexture");
        getInteger = CoreProc<GLGetIntegervFn>("glGetIntegerv");
        texParameter = CoreProc<GLTexParameteriFn>("glTexParameteri");
        getError = CoreProc<GLGetErrorFn>("glGetError");
        finish = CoreProc<GLFinishFn>("glFinish");
        return bindTexture && getInteger && texParameter && getError && finish;
    }
};

struct ShareRecord {
    ComPtr<IUnknown> identity;
    HANDLE handle = nullptr;
    bool owned = false;

    ~ShareRecord() {
        if (owned && handle)
            CloseHandle(handle);
    }
};

struct Device {
    std::mutex operationMutex;
    HANDLE native = nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IUnknown> identity;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> eventQuery;
    LUID luid = {};
    unsigned long long id = 0;
};

struct Object {
    std::shared_ptr<Device> device;
    HANDLE native = nullptr;
    ComPtr<ID3D11Texture2D> original;
    ComPtr<ID3D11Texture2D> bridge;
    ComPtr<IDXGIKeyedMutex> originalMutex;
    D3D11_TEXTURE2D_DESC desc = {};
    GLuint publicName = 0;
    GLenum type = 0;
    GLenum access = WGL_ACCESS_READ_ONLY_NV;
    bool bridgeMode = false;
    bool intelContext = false;
    std::atomic<bool> locked{false};
    bool originalMutexLocked = false;
    unsigned long long id = 0;
    std::atomic<unsigned long long> frames{0};
    std::atomic<unsigned long long> copies{0};
};

thread_local std::vector<std::weak_ptr<Object>> G_THREAD_LOCKED_OBJECTS;

struct Registry {
    std::mutex mutex;
    std::unordered_map<HANDLE, std::shared_ptr<Device>> devices;
    std::unordered_map<HANDLE, std::shared_ptr<Object>> objects;
    std::unordered_map<IUnknown *, std::shared_ptr<ShareRecord>> shares;
    std::atomic<unsigned long long> nextId{1};
};

Registry &GetRegistry() {
    static Registry *value = new Registry;
    return *value;
}

bool IsValidAccess(GLenum access) {
    return access == WGL_ACCESS_READ_ONLY_NV || access == WGL_ACCESS_READ_WRITE_NV || access == WGL_ACCESS_WRITE_DISCARD_NV;
}

bool IsCurrentContextIntel() {
    const auto getString = CoreProc<GLGetStringFn>("glGetString");
    const auto *vendor = getString ? getString(GL_VENDOR) : nullptr;
    return vendor && std::strstr(reinterpret_cast<const char *>(vendor), "Intel") != nullptr;
}

const char *AccessName(GLenum access) {
    switch (access) {
    case WGL_ACCESS_READ_ONLY_NV:
        return "READ_ONLY";
    case WGL_ACCESS_READ_WRITE_NV:
        return "READ_WRITE";
    case WGL_ACCESS_WRITE_DISCARD_NV:
        return "WRITE_DISCARD";
    default:
        return "INVALID";
    }
}

bool IsDirectModeForced() {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("INTEL_OPENGL_VR_FIX_FORCE_DIRECT", value, static_cast<DWORD>(_countof(value)));
    return length > 0 && length < _countof(value) && value[0] != '0';
}

std::shared_ptr<Device> FindDevice(HANDLE handle) {
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.devices.find(handle);
    return found == state.devices.end() ? nullptr : found->second;
}

std::shared_ptr<Object> FindObject(HANDLE handle) {
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.objects.find(handle);
    return found == state.objects.end() ? nullptr : found->second;
}

bool IsDeviceActive(const std::shared_ptr<Device> &device) {
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.devices.find(reinterpret_cast<HANDLE>(device.get()));
    return found != state.devices.end() && found->second == device;
}

bool IsObjectActive(const std::shared_ptr<Object> &object) {
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.objects.find(reinterpret_cast<HANDLE>(object.get()));
    return found != state.objects.end() && found->second == object;
}

ComPtr<IUnknown> QueryIdentity(void *object) {
    ComPtr<IUnknown> identity;
    if (object) {
        reinterpret_cast<IUnknown *>(object)->QueryInterface(IID_PPV_ARGS(&identity));
    }
    return identity;
}

std::shared_ptr<ShareRecord> TakeShare(IUnknown *identity) {
    if (!identity)
        return nullptr;
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.shares.find(identity);
    if (found == state.shares.end())
        return nullptr;
    std::shared_ptr<ShareRecord> share = found->second;
    state.shares.erase(found);
    return share;
}

void RestoreShare(const std::shared_ptr<ShareRecord> &share) {
    if (!share || !share->identity)
        return;
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.shares[share->identity.Get()] = share;
}

void ClearGLErrors(const GLAPI &gl) {
    for (unsigned i = 0; i < 16 && gl.getError() != GL_NO_ERROR; ++i) {
    }
}

bool ClampRegisteredTexture(const GLAPI &gl, GLuint name, UINT mipLevels) {
    if (!name || mipLevels == 0)
        return false;
    GLint previous = 0;
    gl.getInteger(GL_TEXTURE_BINDING_2D_VALUE, &previous);
    ClearGLErrors(gl);
    gl.bindTexture(GL_TEXTURE_2D, name);
    gl.texParameter(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL_VALUE, static_cast<GLint>(mipLevels - 1));
    const GLenum error = gl.getError();
    gl.bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    return error == GL_NO_ERROR;
}

DWORD WaitD3D(const Device &device) {
    device.context->End(device.eventQuery.Get());
    device.context->Flush();
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const HRESULT hr = device.context->GetData(device.eventQuery.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
            return ERROR_SUCCESS;
        if (FAILED(hr))
            return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ? ERROR_DEVICE_NOT_AVAILABLE : ERROR_GEN_FAILURE;
        if (GetTickCount64() - start >= INTEROP_WAIT_MS)
            return ERROR_TIMEOUT;
        SwitchToThread();
    }
}

bool IsSameDevice(ID3D11Texture2D *texture, const Device &device) {
    ComPtr<ID3D11Device> textureDevice;
    ComPtr<IUnknown> identity;
    texture->GetDevice(&textureDevice);
    if (!textureDevice || FAILED(textureDevice.As(&identity)))
        return false;
    return identity.Get() == device.identity.Get();
}

std::shared_ptr<Object> CreateBridgeObject(const std::shared_ptr<Device> &device, ID3D11Texture2D *original, GLuint publicName, GLenum type,
                                           GLenum access, DWORD *failureError) {
    if (failureError)
        *failureError = ERROR_NOT_SUPPORTED;
    if (!device || !original || !publicName || type != GL_TEXTURE_2D)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    original->GetDesc(&desc);
    if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Usage != D3D11_USAGE_DEFAULT)
        return nullptr;

    GLAPI gl;
    if (!gl.Load())
        return nullptr;

    D3D11_TEXTURE2D_DESC bridgeDesc = desc;
    bridgeDesc.Usage = D3D11_USAGE_DEFAULT;
    bridgeDesc.CPUAccessFlags = 0;
    bridgeDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> bridge;
    HRESULT hr = device->d3d->CreateTexture2D(&bridgeDesc, nullptr, &bridge);
    if (FAILED(hr) || !bridge) {
        if (failureError)
            *failureError = hr == E_OUTOFMEMORY ? ERROR_NOT_ENOUGH_MEMORY : ERROR_NOT_SUPPORTED;
        return nullptr;
    }

    ComPtr<IDXGIResource> sharedResource;
    HANDLE kmtHandle = nullptr;
    hr = bridge.As(&sharedResource);
    if (FAILED(hr) || !sharedResource || FAILED(sharedResource->GetSharedHandle(&kmtHandle)) || !kmtHandle) {
        return nullptr;
    }

    const auto nativeSet = NativeProc<DXSetShareFn>("wglDXSetResourceShareHandleNV");
    const auto nativeRegister = NativeProc<DXRegisterObjectFn>("wglDXRegisterObjectNV");
    const auto nativeUnregister = NativeProc<DXUnregisterObjectFn>("wglDXUnregisterObjectNV");
    if (!nativeSet || !nativeRegister || !nativeUnregister || !nativeSet(bridge.Get(), kmtHandle)) {
        const DWORD error = GetLastError();
        if (failureError)
            *failureError = error ? error : ERROR_NOT_SUPPORTED;
        return nullptr;
    }

    HANDLE nativeObject = nativeRegister(device->native, bridge.Get(), publicName, type, access);
    if (!nativeObject) {
        const DWORD error = GetLastError();
        if (failureError)
            *failureError = error ? error : ERROR_NOT_SUPPORTED;
        return nullptr;
    }

    if (!ClampRegisteredTexture(gl, publicName, desc.MipLevels)) {
        nativeUnregister(device->native, nativeObject);
        if (failureError)
            *failureError = ERROR_INVALID_OPERATION;
        return nullptr;
    }

    auto object = std::make_shared<Object>();
    object->device = device;
    object->native = nativeObject;
    object->original = original;
    object->bridge = bridge;
    original->QueryInterface(IID_PPV_ARGS(&object->originalMutex));
    object->desc = desc;
    object->publicName = publicName;
    object->type = type;
    object->access = access;
    object->bridgeMode = true;
    object->intelContext = IsCurrentContextIntel();
    object->id = GetRegistry().nextId.fetch_add(1);
    IntelOpenGLVRFix::LogMessage("Register bridge result=OK object=%llu device=%llu public_gl=%u "
                                 "original=%p bridge=%p kmt=%p format=%u size=%ux%u "
                                 "source_misc=0x%x bridge_misc=0x%x access=%s",
                                 object->id, device->id, publicName, original, bridge.Get(), kmtHandle, static_cast<unsigned>(desc.Format),
                                 desc.Width, desc.Height, desc.MiscFlags, bridgeDesc.MiscFlags, AccessName(access));
    return object;
}

std::shared_ptr<Object> CreateDirectObject(const std::shared_ptr<Device> &device, ID3D11Texture2D *original, GLuint name, GLenum type,
                                           GLenum access, const std::shared_ptr<ShareRecord> &share, DWORD *failureError) {
    const auto nativeSet = NativeProc<DXSetShareFn>("wglDXSetResourceShareHandleNV");
    const auto nativeRegister = NativeProc<DXRegisterObjectFn>("wglDXRegisterObjectNV");
    if (!nativeRegister)
        return nullptr;
    if (share && (!nativeSet || !nativeSet(original, share->handle))) {
        if (failureError)
            *failureError = GetLastError();
        return nullptr;
    }

    HANDLE nativeObject = nativeRegister(device->native, original, name, type, access);
    if (!nativeObject) {
        if (failureError)
            *failureError = GetLastError();
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    original->GetDesc(&desc);
    if (type == GL_TEXTURE_2D) {
        GLAPI gl;
        if (gl.Load())
            ClampRegisteredTexture(gl, name, std::max(1u, desc.MipLevels));
    }

    auto object = std::make_shared<Object>();
    object->device = device;
    object->native = nativeObject;
    object->original = original;
    object->desc = desc;
    object->publicName = name;
    object->type = type;
    object->access = access;
    object->intelContext = IsCurrentContextIntel();
    object->id = GetRegistry().nextId.fetch_add(1);
    IntelOpenGLVRFix::LogMessage("Register direct result=OK object=%llu device=%llu gl=%u original=%p "
                                 "format=%u size=%ux%u misc=0x%x access=%s",
                                 object->id, device->id, name, original, static_cast<unsigned>(desc.Format), desc.Width, desc.Height,
                                 desc.MiscFlags, AccessName(access));
    return object;
}

bool CollectObjects(const std::shared_ptr<Device> &device, GLint count, HANDLE *handles, std::vector<std::shared_ptr<Object>> *objects,
                    bool requireLocked) {
    if (!objects)
        return false;
    objects->clear();
    Registry &state = GetRegistry();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (GLint i = 0; i < count; ++i) {
        const auto found = state.objects.find(handles[i]);
        if (found == state.objects.end() || found->second->device != device || found->second->locked != requireLocked)
            return false;
        if (std::find(objects->begin(), objects->end(), found->second) != objects->end())
            return false;
        objects->push_back(found->second);
    }
    return true;
}

bool ShouldLogFrame(Object &object) {
    const unsigned long long frame = object.frames.fetch_add(1) + 1;
    return frame <= 8 || frame % 300 == 0;
}

void RememberThreadLocks(const std::vector<std::shared_ptr<Object>> &objects) {
    G_THREAD_LOCKED_OBJECTS.erase(std::remove_if(G_THREAD_LOCKED_OBJECTS.begin(), G_THREAD_LOCKED_OBJECTS.end(),
                                                 [](const std::weak_ptr<Object> &entry) { return entry.expired(); }),
                                  G_THREAD_LOCKED_OBJECTS.end());
    for (const auto &object : objects) {
        const auto found = std::find_if(G_THREAD_LOCKED_OBJECTS.begin(), G_THREAD_LOCKED_OBJECTS.end(),
                                        [&object](const std::weak_ptr<Object> &entry) { return entry.lock() == object; });
        if (found == G_THREAD_LOCKED_OBJECTS.end())
            G_THREAD_LOCKED_OBJECTS.emplace_back(object);
    }
}

void ForgetThreadLocks(const std::vector<std::shared_ptr<Object>> &objects) {
    G_THREAD_LOCKED_OBJECTS.erase(std::remove_if(G_THREAD_LOCKED_OBJECTS.begin(), G_THREAD_LOCKED_OBJECTS.end(),
                                                 [&objects](const std::weak_ptr<Object> &entry) {
                                                     const auto object = entry.lock();
                                                     return !object || std::find(objects.begin(), objects.end(), object) != objects.end();
                                                 }),
                                  G_THREAD_LOCKED_OBJECTS.end());
}

std::shared_ptr<Object> FindThreadCopyDestination(GLuint name, GLenum type, GLint level, GLint xOffset, GLint yOffset, GLint zOffset,
                                                  GLsizei width, GLsizei height, GLsizei depth) {
    std::shared_ptr<Object> result;
    for (auto iterator = G_THREAD_LOCKED_OBJECTS.begin(); iterator != G_THREAD_LOCKED_OBJECTS.end();) {
        const auto object = iterator->lock();
        if (!object) {
            iterator = G_THREAD_LOCKED_OBJECTS.erase(iterator);
            continue;
        }
        ++iterator;
        if (!object->locked.load() || object->publicName != name || object->type != type || !object->intelContext ||
            object->access == WGL_ACCESS_READ_ONLY_NV || level != 0 || xOffset != 0 || yOffset != 0 || zOffset != 0 || depth != 1 ||
            object->desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || object->desc.ArraySize != 1 || object->desc.MipLevels != 1 ||
            object->desc.SampleDesc.Count != 1 || width != static_cast<GLsizei>(object->desc.Width) ||
            height != static_cast<GLsizei>(object->desc.Height))
            continue;
        if (result)
            return nullptr;
        result = object;
    }
    return result;
}

bool CopyImageWithFramebuffer(GLuint sourceName, GLuint destinationName, GLsizei width, GLsizei height) {
    const auto getInteger = CoreProc<GLGetIntegervFn>("glGetIntegerv");
    const auto getLevel = CoreProc<GLGetTexLevelParameterivFn>("glGetTexLevelParameteriv");
    const auto isTexture = CoreProc<GLIsTextureFn>("glIsTexture");
    const auto isEnabled = CoreProc<GLIsEnabledFn>("glIsEnabled");
    const auto enable = CoreProc<GLEnableFn>("glEnable");
    const auto disable = CoreProc<GLDisableFn>("glDisable");
    const auto genFramebuffers = NativeProc<GLGenFramebuffersFn>("glGenFramebuffers");
    const auto deleteFramebuffers = NativeProc<GLDeleteFramebuffersFn>("glDeleteFramebuffers");
    const auto bindFramebuffer = NativeProc<GLBindFramebufferFn>("glBindFramebuffer");
    const auto attachTexture = NativeProc<GLFramebufferTexture2DFn>("glFramebufferTexture2D");
    const auto checkFramebuffer = NativeProc<GLCheckFramebufferStatusFn>("glCheckFramebufferStatus");
    const auto blitFramebuffer = NativeProc<GLBlitFramebufferFn>("glBlitFramebuffer");
    if (!getInteger || !getLevel || !isTexture || !isEnabled || !enable || !disable || !genFramebuffers || !deleteFramebuffers ||
        !bindFramebuffer || !attachTexture || !checkFramebuffer || !blitFramebuffer)
        return false;

    if (isTexture(sourceName) != GL_TRUE || isTexture(destinationName) != GL_TRUE)
        return false;

    GLint previousTexture = 0;
    GLint previousRead = 0;
    GLint previousDraw = 0;
    getInteger(GL_TEXTURE_BINDING_2D_VALUE, &previousTexture);
    getInteger(GL_READ_FRAMEBUFFER_BINDING_VALUE, &previousRead);
    getInteger(GL_DRAW_FRAMEBUFFER_BINDING_VALUE, &previousDraw);
    const bool scissorEnabled = isEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool srgbEnabled = isEnabled(GL_FRAMEBUFFER_SRGB_VALUE) == GL_TRUE;

    const auto bindTexture = CoreProc<GLBindTextureFn>("glBindTexture");
    if (!bindTexture)
        return false;
    GLint sourceFormat = 0;
    GLint sourceWidth = 0;
    GLint sourceHeight = 0;
    bindTexture(GL_TEXTURE_2D, sourceName);
    getLevel(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT_VALUE, &sourceFormat);
    getLevel(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH_VALUE, &sourceWidth);
    getLevel(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT_VALUE, &sourceHeight);
    bindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    const auto compatibleFormat = [](GLint format) {
        return format == static_cast<GLint>(GL_RGBA) || format == static_cast<GLint>(GL_RGBA8_VALUE) ||
               format == static_cast<GLint>(GL_SRGB_ALPHA_VALUE) || format == static_cast<GLint>(GL_SRGB8_ALPHA8_VALUE);
    };
    if (!compatibleFormat(sourceFormat) || sourceWidth != width || sourceHeight != height)
        return false;

    GLuint framebuffers[2] = {};
    genFramebuffers(2, framebuffers);
    if (!framebuffers[0] || !framebuffers[1]) {
        if (framebuffers[0] || framebuffers[1])
            deleteFramebuffers(2, framebuffers);
        return false;
    }

    bindFramebuffer(GL_READ_FRAMEBUFFER_VALUE, framebuffers[0]);
    attachTexture(GL_READ_FRAMEBUFFER_VALUE, GL_COLOR_ATTACHMENT0_VALUE, GL_TEXTURE_2D, sourceName, 0);
    bindFramebuffer(GL_DRAW_FRAMEBUFFER_VALUE, framebuffers[1]);
    attachTexture(GL_DRAW_FRAMEBUFFER_VALUE, GL_COLOR_ATTACHMENT0_VALUE, GL_TEXTURE_2D, destinationName, 0);
    const bool complete = checkFramebuffer(GL_READ_FRAMEBUFFER_VALUE) == GL_FRAMEBUFFER_COMPLETE_VALUE &&
                          checkFramebuffer(GL_DRAW_FRAMEBUFFER_VALUE) == GL_FRAMEBUFFER_COMPLETE_VALUE;
    if (complete) {
        if (scissorEnabled)
            disable(GL_SCISSOR_TEST);
        if (srgbEnabled)
            disable(GL_FRAMEBUFFER_SRGB_VALUE);
        blitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (srgbEnabled)
            enable(GL_FRAMEBUFFER_SRGB_VALUE);
        if (scissorEnabled)
            enable(GL_SCISSOR_TEST);
    }

    bindFramebuffer(GL_READ_FRAMEBUFFER_VALUE, static_cast<GLuint>(previousRead));
    bindFramebuffer(GL_DRAW_FRAMEBUFFER_VALUE, static_cast<GLuint>(previousDraw));
    deleteFramebuffers(2, framebuffers);
    return complete;
}

const char *AddInteropExtensions(const char *source) {
    if (!source)
        return nullptr;
    thread_local std::string result;
    result = source;
    const auto add = [](std::string *value, const char *extension) {
        if (value->find(extension) != std::string::npos)
            return;
        if (!value->empty() && value->back() != ' ')
            value->push_back(' ');
        value->append(extension);
    };
    add(&result, "WGL_NV_DX_interop");
    add(&result, "WGL_NV_DX_interop2");
    return result.c_str();
}

} // namespace

extern "C" BOOL WINAPI wglDXSetResourceShareHandleNV(void *dxObject, HANDLE shareHandle) {
    if (!dxObject || !shareHandle) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ComPtr<IUnknown> identity = QueryIdentity(dxObject);
    if (!identity) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    auto share = std::make_shared<ShareRecord>();
    share->identity = identity;
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), shareHandle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        share->handle = duplicate;
        share->owned = true;
    } else {
        share->handle = shareHandle;
    }

    Registry &state = GetRegistry();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.shares[identity.Get()] = share;
    }
    IntelOpenGLVRFix::LogMessage("SetShare result=OK object=%p handle=%p duplicate=%d", dxObject, shareHandle, share->owned ? 1 : 0);
    return TRUE;
}

extern "C" HANDLE WINAPI wglDXOpenDeviceNV(void *dxDevice) {
    if (!dxDevice) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ComPtr<ID3D11Device> d3d;
    if (FAILED(reinterpret_cast<IUnknown *>(dxDevice)->QueryInterface(IID_PPV_ARGS(&d3d))) || !d3d) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const auto nativeOpen = NativeProc<DXOpenDeviceFn>("wglDXOpenDeviceNV");
    if (!nativeOpen) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    HANDLE nativeHandle = nativeOpen(d3d.Get());
    if (!nativeHandle)
        return nullptr;

    auto device = std::make_shared<Device>();
    device->native = nativeHandle;
    device->d3d = d3d;
    d3d.As(&device->identity);
    d3d->GetImmediateContext(&device->context);
    D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
    const HRESULT queryHr = d3d->CreateQuery(&queryDesc, &device->eventQuery);
    if (!device->identity || !device->context || FAILED(queryHr) || !device->eventQuery) {
        const auto nativeClose = NativeProc<DXCloseDeviceFn>("wglDXCloseDeviceNV");
        if (nativeClose)
            nativeClose(nativeHandle);
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc = {};
    if (SUCCEEDED(d3d.As(&dxgiDevice)) && dxgiDevice && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
        SUCCEEDED(adapter->GetDesc(&adapterDesc)))
        device->luid = adapterDesc.AdapterLuid;
    device->id = GetRegistry().nextId.fetch_add(1);

    HANDLE publicHandle = reinterpret_cast<HANDLE>(device.get());
    {
        Registry &state = GetRegistry();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.emplace(publicHandle, device);
    }
    IntelOpenGLVRFix::LogMessage("OpenDevice result=OK device=%llu public=%p native=%p d3d=%p "
                                 "luid=%08lx:%08lx",
                                 device->id, publicHandle, nativeHandle, d3d.Get(), static_cast<unsigned long>(device->luid.HighPart),
                                 static_cast<unsigned long>(device->luid.LowPart));
    return publicHandle;
}

extern "C" BOOL WINAPI wglDXCloseDeviceNV(HANDLE handle) {
    auto device = FindDevice(handle);
    if (!device) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard<std::mutex> operation(device->operationMutex);
    Registry &state = GetRegistry();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto active = state.devices.find(handle);
        if (active == state.devices.end() || active->second != device) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        for (const auto &entry : state.objects) {
            if (entry.second->device == device) {
                SetLastError(ERROR_BUSY);
                return FALSE;
            }
        }
    }

    const auto nativeClose = NativeProc<DXCloseDeviceFn>("wglDXCloseDeviceNV");
    if (!nativeClose || !nativeClose(device->native))
        return FALSE;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.erase(handle);
    }
    IntelOpenGLVRFix::LogMessage("CloseDevice result=OK device=%llu", device->id);
    return TRUE;
}

extern "C" HANDLE WINAPI wglDXRegisterObjectNV(HANDLE deviceHandle, void *dxObject, GLuint name, GLenum type, GLenum access) {
    if (!dxObject || !name || !IsValidAccess(access)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto device = FindDevice(deviceHandle);
    if (!device) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    std::lock_guard<std::mutex> operation(device->operationMutex);
    if (!IsDeviceActive(device)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(reinterpret_cast<IUnknown *>(dxObject)->QueryInterface(IID_PPV_ARGS(&texture))) || !texture ||
        !IsSameDevice(texture.Get(), *device)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ComPtr<IUnknown> identity;
    texture.As(&identity);
    auto share = TakeShare(identity.Get());

    DWORD bridgeError = ERROR_NOT_SUPPORTED;
    std::shared_ptr<Object> object;
    if (!IsDirectModeForced()) {
        object = CreateBridgeObject(device, texture.Get(), name, type, access, &bridgeError);
    }
    if (!object) {
        IntelOpenGLVRFix::LogMessage("Register bridge result=FAIL device=%llu gl=%u error=%lu; "
                                     "trying direct",
                                     device->id, name, static_cast<unsigned long>(bridgeError));
        object = CreateDirectObject(device, texture.Get(), name, type, access, share, &bridgeError);
    }
    if (!object) {
        RestoreShare(share);
        SetLastError(bridgeError ? bridgeError : ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    HANDLE publicHandle = reinterpret_cast<HANDLE>(object.get());
    {
        Registry &state = GetRegistry();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.objects.emplace(publicHandle, object);
    }
    return publicHandle;
}

extern "C" BOOL WINAPI wglDXUnregisterObjectNV(HANDLE deviceHandle, HANDLE objectHandle) {
    auto device = FindDevice(deviceHandle);
    auto object = FindObject(objectHandle);
    if (!device || !object || object->device != device) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard<std::mutex> operation(device->operationMutex);
    if (!IsDeviceActive(device) || !IsObjectActive(object) || object->locked) {
        SetLastError(object->locked ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return FALSE;
    }

    const auto nativeUnregister = NativeProc<DXUnregisterObjectFn>("wglDXUnregisterObjectNV");
    if (!nativeUnregister || !nativeUnregister(device->native, object->native))
        return FALSE;

    {
        Registry &state = GetRegistry();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.objects.erase(objectHandle);
    }
    IntelOpenGLVRFix::LogMessage("Unregister result=OK object=%llu device=%llu bridge=%d", object->id, device->id,
                                 object->bridgeMode ? 1 : 0);
    return TRUE;
}

extern "C" BOOL WINAPI wglDXObjectAccessNV(HANDLE objectHandle, GLenum access) {
    if (!IsValidAccess(access)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto object = FindObject(objectHandle);
    if (!object) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    auto device = object->device;
    std::lock_guard<std::mutex> operation(device->operationMutex);
    if (!IsObjectActive(object) || object->locked) {
        SetLastError(object->locked ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const auto nativeAccess = NativeProc<DXObjectAccessFn>("wglDXObjectAccessNV");
    if (!nativeAccess || !nativeAccess(object->native, access))
        return FALSE;
    object->access = access;
    return TRUE;
}

extern "C" BOOL WINAPI wglDXLockObjectsNV(HANDLE deviceHandle, GLint count, HANDLE *handles) {
    if (count < 0 || (count > 0 && !handles)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto device = FindDevice(deviceHandle);
    if (!device) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard<std::mutex> operation(device->operationMutex);
    if (!IsDeviceActive(device)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (count == 0)
        return TRUE;

    std::vector<std::shared_ptr<Object>> objects;
    if (!CollectObjects(device, count, handles, &objects, false)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    for (auto &object : objects) {
        if (object->bridgeMode && object->originalMutex) {
            const HRESULT hr = object->originalMutex->AcquireSync(0, INTEROP_WAIT_MS);
            if (hr != S_OK) {
                for (auto &rollback : objects) {
                    if (rollback->originalMutexLocked) {
                        rollback->originalMutex->ReleaseSync(0);
                        rollback->originalMutexLocked = false;
                    }
                }
                SetLastError(hr == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_LOCK_FAILED);
                return FALSE;
            }
            object->originalMutexLocked = true;
        }
        if (object->bridgeMode && object->access != WGL_ACCESS_WRITE_DISCARD_NV) {
            device->context->CopyResource(object->bridge.Get(), object->original.Get());
        }
    }

    bool hasIncomingCopy = false;
    for (const auto &object : objects) {
        hasIncomingCopy = hasIncomingCopy || (object->bridgeMode && object->access != WGL_ACCESS_WRITE_DISCARD_NV);
    }
    if (hasIncomingCopy) {
        const DWORD waitError = WaitD3D(*device);
        if (waitError != ERROR_SUCCESS) {
            for (auto &object : objects) {
                if (object->originalMutexLocked) {
                    object->originalMutex->ReleaseSync(0);
                    object->originalMutexLocked = false;
                }
            }
            SetLastError(waitError);
            return FALSE;
        }
    }

    std::vector<HANDLE> nativeHandles;
    nativeHandles.reserve(objects.size());
    for (const auto &object : objects)
        nativeHandles.push_back(object->native);
    const auto nativeLock = NativeProc<DXLockObjectsFn>("wglDXLockObjectsNV");
    if (!nativeLock || !nativeLock(device->native, count, nativeHandles.data())) {
        const DWORD error = GetLastError();
        for (auto &object : objects) {
            if (object->originalMutexLocked) {
                object->originalMutex->ReleaseSync(0);
                object->originalMutexLocked = false;
            }
        }
        SetLastError(error);
        return FALSE;
    }

    for (auto &object : objects)
        object->locked = true;
    RememberThreadLocks(objects);
    return TRUE;
}

extern "C" BOOL WINAPI wglDXUnlockObjectsNV(HANDLE deviceHandle, GLint count, HANDLE *handles) {
    if (count < 0 || (count > 0 && !handles)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto device = FindDevice(deviceHandle);
    if (!device) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard<std::mutex> operation(device->operationMutex);
    if (!IsDeviceActive(device)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (count == 0)
        return TRUE;

    std::vector<std::shared_ptr<Object>> objects;
    if (!CollectObjects(device, count, handles, &objects, true)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    bool result = true;
    DWORD resultError = ERROR_SUCCESS;
    bool writableBridge = false;
    for (const auto &object : objects) {
        writableBridge = writableBridge || (object->bridgeMode && object->access != WGL_ACCESS_READ_ONLY_NV);
    }
    if (writableBridge) {
        GLAPI gl;
        if (!gl.Load()) {
            SetLastError(ERROR_INVALID_OPERATION);
            return FALSE;
        }
        gl.finish();
    }

    std::vector<HANDLE> nativeHandles;
    nativeHandles.reserve(objects.size());
    for (const auto &object : objects)
        nativeHandles.push_back(object->native);
    const auto nativeUnlock = NativeProc<DXUnlockObjectsFn>("wglDXUnlockObjectsNV");
    if (!nativeUnlock || !nativeUnlock(device->native, count, nativeHandles.data())) {
        SetLastError(GetLastError());
        return FALSE;
    }

    bool outgoingCopy = false;
    for (const auto &object : objects) {
        if (object->bridgeMode && object->access != WGL_ACCESS_READ_ONLY_NV) {
            device->context->CopyResource(object->original.Get(), object->bridge.Get());
            outgoingCopy = true;
        }
    }
    if (outgoingCopy) {
        const DWORD waitError = WaitD3D(*device);
        if (waitError != ERROR_SUCCESS) {
            result = false;
            resultError = waitError;
        }
    }

    for (auto &object : objects) {
        object->locked = false;
        if (object->originalMutexLocked) {
            const HRESULT hr = object->originalMutex->ReleaseSync(0);
            object->originalMutexLocked = false;
            if (hr != S_OK && result) {
                result = false;
                resultError = ERROR_LOCK_FAILED;
            }
        }
        if (ShouldLogFrame(*object)) {
            IntelOpenGLVRFix::LogMessage("Unlock object=%llu device=%llu bridge=%d access=%s result=%s", object->id, device->id,
                                         object->bridgeMode ? 1 : 0, AccessName(object->access), result ? "OK" : "FAIL");
        }
    }
    ForgetThreadLocks(objects);
    if (!result)
        SetLastError(resultError);
    return result ? TRUE : FALSE;
}

extern "C" void APIENTRY ProxyGLCopyImageSubData(GLuint sourceName, GLenum sourceTarget, GLint sourceLevel, GLint sourceX, GLint sourceY,
                                                 GLint sourceZ, GLuint destinationName, GLenum destinationTarget, GLint destinationLevel,
                                                 GLint destinationX, GLint destinationY, GLint destinationZ, GLsizei width, GLsizei height,
                                                 GLsizei depth) {
    const auto native = NativeProc<GLCopyImageSubDataFn>("glCopyImageSubData");
    if (!native)
        return;

    const auto object = sourceName != destinationName && sourceTarget == GL_TEXTURE_2D && destinationTarget == GL_TEXTURE_2D &&
                                sourceLevel == 0 && sourceX == 0 && sourceY == 0 && sourceZ == 0
                            ? FindThreadCopyDestination(destinationName, destinationTarget, destinationLevel, destinationX, destinationY,
                                                        destinationZ, width, height, depth)
                            : nullptr;
    if (!object) {
        native(sourceName, sourceTarget, sourceLevel, sourceX, sourceY, sourceZ, destinationName, destinationTarget, destinationLevel,
               destinationX, destinationY, destinationZ, width, height, depth);
        return;
    }

    if (!CopyImageWithFramebuffer(sourceName, destinationName, width, height)) {
        IntelOpenGLVRFix::LogMessage("CopyImage bridge result=FAIL object=%llu src_gl=%u dst_gl=%u "
                                     "size=%dx%d; trying native",
                                     object->id, sourceName, destinationName, width, height);
        native(sourceName, sourceTarget, sourceLevel, sourceX, sourceY, sourceZ, destinationName, destinationTarget, destinationLevel,
               destinationX, destinationY, destinationZ, width, height, depth);
        return;
    }

    const unsigned long long copy = object->copies.fetch_add(1) + 1;
    if (copy <= 8 || copy % 300 == 0) {
        IntelOpenGLVRFix::LogMessage("CopyImage bridge result=OK object=%llu copy=%llu src_gl=%u "
                                     "dst_gl=%u size=%dx%d backend=FBO_BLIT",
                                     object->id, copy, sourceName, destinationName, width, height);
    }
}

extern "C" const char *WINAPI ProxyWGLGetExtensionsStringARB(HDC dc) {
    const auto native = NativeProc<WGLExtensionsARBFn>("wglGetExtensionsStringARB");
    return native ? AddInteropExtensions(native(dc)) : nullptr;
}

extern "C" const char *WINAPI ProxyWGLGetExtensionsStringEXT() {
    const auto native = NativeProc<WGLExtensionsEXTFn>("wglGetExtensionsStringEXT");
    return native ? AddInteropExtensions(native()) : nullptr;
}

namespace IntelOpenGLVRFix {

PROC InteropProcAddress(const char *name) {
    if (!name)
        return nullptr;
    if (std::strcmp(name, "glCopyImageSubData") == 0 && !SystemWGLProc(name))
        return nullptr;
    struct Entry {
        const char *name;
        PROC proc;
    };
    const Entry entries[] = {
        {"wglDXSetResourceShareHandleNV", reinterpret_cast<PROC>(wglDXSetResourceShareHandleNV)},
        {"wglDXOpenDeviceNV", reinterpret_cast<PROC>(wglDXOpenDeviceNV)},
        {"wglDXCloseDeviceNV", reinterpret_cast<PROC>(wglDXCloseDeviceNV)},
        {"wglDXRegisterObjectNV", reinterpret_cast<PROC>(wglDXRegisterObjectNV)},
        {"wglDXUnregisterObjectNV", reinterpret_cast<PROC>(wglDXUnregisterObjectNV)},
        {"wglDXObjectAccessNV", reinterpret_cast<PROC>(wglDXObjectAccessNV)},
        {"wglDXLockObjectsNV", reinterpret_cast<PROC>(wglDXLockObjectsNV)},
        {"wglDXUnlockObjectsNV", reinterpret_cast<PROC>(wglDXUnlockObjectsNV)},
        {"wglGetExtensionsStringARB", reinterpret_cast<PROC>(ProxyWGLGetExtensionsStringARB)},
        {"wglGetExtensionsStringEXT", reinterpret_cast<PROC>(ProxyWGLGetExtensionsStringEXT)},
        {"glCopyImageSubData", reinterpret_cast<PROC>(ProxyGLCopyImageSubData)},
    };
    for (const Entry &entry : entries) {
        if (std::strcmp(name, entry.name) == 0)
            return entry.proc;
    }
    return nullptr;
}

} // namespace IntelOpenGLVRFix
