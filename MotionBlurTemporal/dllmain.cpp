// MotionBlurTemporal.dll  -  x64, DirectX 11, MinHook
// Hook de IDXGISwapChain::Present / Present1 / ResizeBuffers.
// En cada Present: copia el backbuffer, mezcla con el historial y escribe el resultado
// a la vez en el backbuffer y en la textura de historial (MRT, ping-pong A/B).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "MinHook.h"
#include "Shader.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// Diagnóstico: los mensajes se ven con DebugView (Sysinternals). Filtra por "MotionBlur".
#include <cstdarg>
static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    OutputDebugStringA("[MotionBlur] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ===================================================================== ajustes
namespace Settings
{
    bool  enabled = true;
    float slider = 0.20f;      // 0..1  (0.20 ~ 240 FPS simulados)
    float shutter = 1.0f;       // 1.0 = 360°, 0.5 = 180° (cine)
    float maxTrail = 0.016f;     // s, tope de estela en modo fluido
    float intensity = 1.0f;       // 0..2  multiplicador de la cantidad de blur (1 = normal)
    constexpr float kMinFps = 12.f, kMaxFps = 500.f;

    inline float FpsToSlider(float fps)
    {
        return std::clamp(std::log(kMaxFps / fps) / std::log(kMaxFps / kMinFps), 0.f, 1.f);
    }
}

// Constant buffer (debe coincidir con cbuffer Params del HLSL, 32 bytes)
struct alignas(16) CBParams
{
    float dt, slider, shutter, maxTrail;
    float minFps, maxFps, gammaSpace, reset;
    float intensity, osdValue, osdAlpha, osdKind;
};
static_assert(sizeof(CBParams) == 48, "CBParams debe ser 48 bytes");

// ==================================================================== renderer
struct Renderer
{
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11Buffer>        cb;
    ComPtr<ID3D11RasterizerState>   rs;
    ComPtr<ID3D11DepthStencilState> ds;
    ComPtr<ID3D11BlendState>        bs;

    ComPtr<ID3D11Texture2D>          cur;           // copia del backbuffer
    ComPtr<ID3D11ShaderResourceView> curSrv;
    ComPtr<ID3D11Texture2D>          hist[2];       // history buffer (ping-pong)
    ComPtr<ID3D11ShaderResourceView> histSrv[2];
    ComPtr<ID3D11RenderTargetView>   histRtv[2];

    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool gammaSpace = false;
    int  readIdx = 0;
    bool needReset = true;

    void Release() { *this = Renderer(); }
};

static Renderer       g_r;
static IDXGISwapChain* g_sc = nullptr;     // swap chain que estamos procesando

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn  oPresent = nullptr;
static Present1Fn oPresent1 = nullptr;
static ResizeFn   oResize = nullptr;

// ================================================================ utilidades
static double NowSec()
{
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "MotionBlur.hlsl", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        return false;
    }
    return true;
}

static DXGI_FORMAT ResolveViewFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static bool IsLinearBlendNeeded(DXGI_FORMAT f)
{
    // Formatos UNORM "gamma" -> mezclar en lineal. sRGB y FLOAT ya son lineales al muestrear.
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
        f == DXGI_FORMAT_R10G10B10A2_UNORM;
}

// ============================================================= guardar estado
// El juego espera su estado intacto tras nuestro Draw. Guardamos/restauramos lo que tocamos.
struct StateBackup
{
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT sampleMask = 0;
    ID3D11DepthStencilState* dss = nullptr; UINT stencilRef = 0;
    ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11Buffer* cb = nullptr;

    void Save(ID3D11DeviceContext* c)
    {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->RSGetViewports(&nvp, vp);
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &sampleMask);
        c->OMGetDepthStencilState(&dss, &stencilRef);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->PSGetShaderResources(0, 2, srv);
        c->PSGetConstantBuffers(0, 1, &cb);
    }

    template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

    void Restore(ID3D11DeviceContext* c)
    {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->RSSetViewports(nvp, vp);
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, sampleMask);
        c->OMSetDepthStencilState(dss, stencilRef);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->PSSetShaderResources(0, 2, srv);
        c->PSSetConstantBuffers(0, 1, &cb);

        for (auto& r : rtv) Rel(r);
        Rel(dsv); Rel(rs); Rel(bs); Rel(dss); Rel(il); Rel(vs); Rel(ps); Rel(gs); Rel(hs); Rel(ds);
        Rel(srv[0]); Rel(srv[1]); Rel(cb);
    }
};

// ============================================================ inicialización
static bool InitResources(Renderer& r, const D3D11_TEXTURE2D_DESC& bbDesc)
{
    r.w = bbDesc.Width; r.h = bbDesc.Height;
    r.fmt = ResolveViewFormat(bbDesc.Format);
    r.gammaSpace = IsLinearBlendNeeded(r.fmt);

    if (!r.vs) {
        ComPtr<ID3DBlob> vsb, psb;
        if (!Compile("VSMain", "vs_5_0", vsb) || !Compile("PSMain", "ps_5_0", psb)) return false;
        if (FAILED(r.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &r.vs))) return false;
        if (FAILED(r.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &r.ps))) return false;

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(CBParams);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(r.dev->CreateBuffer(&cbd, nullptr, &r.cb))) return false;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(r.dev->CreateRasterizerState(&rd, &r.rs))) return false;

        D3D11_DEPTH_STENCIL_DESC dd{};   // DepthEnable = FALSE
        if (FAILED(r.dev->CreateDepthStencilState(&dd, &r.ds))) return false;

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(r.dev->CreateBlendState(&bd, &r.bs))) return false;
    }

    // Textura "actual" (solo lectura) + 2 texturas de historial (lectura/escritura)
    D3D11_TEXTURE2D_DESC td{};
    td.Width = r.w; td.Height = r.h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = r.fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;

    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &r.cur))) return false;
    if (FAILED(r.dev->CreateShaderResourceView(r.cur.Get(), nullptr, &r.curSrv))) return false;

    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &r.hist[i]))) return false;
        if (FAILED(r.dev->CreateShaderResourceView(r.hist[i].Get(), nullptr, &r.histSrv[i]))) return false;
        if (FAILED(r.dev->CreateRenderTargetView(r.hist[i].Get(), nullptr, &r.histRtv[i]))) return false;
    }

    r.readIdx = 0;
    r.needReset = true;
    return true;
}

// ================================================================== hotkeys
static bool Edge(int vk)
{
    static bool prev[256] = {};
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool e = down && !prev[vk];
    prev[vk] = down;
    return e;
}

// Barra en pantalla (OSD) que se muestra ~1.5 s tras cambiar un valor
static float g_osdTimer = 0.f, g_osdValue = 0.f, g_osdKind = 0.f;   // kind: 0 = FPS simulados, 1 = intensidad
static void ShowOsd(float kind, float value01) { g_osdKind = kind; g_osdValue = value01; g_osdTimer = 1.5f; }

static void PollHotkeys(float dt)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;

    if (Edge(VK_F6)) Settings::enabled = !Settings::enabled;

    // F7 / F8: FPS simulados (carácter del blur)
    if (GetAsyncKeyState(VK_F7) & 0x8000) { Settings::slider = std::max(0.f, Settings::slider - 0.4f * dt); ShowOsd(0.f, Settings::slider); }
    if (GetAsyncKeyState(VK_F8) & 0x8000) { Settings::slider = std::min(1.f, Settings::slider + 0.4f * dt); ShowOsd(0.f, Settings::slider); }

    // Re Pág / Av Pág: INTENSIDAD (cantidad de blur) 0%..200%.  Inicio: volver a 100%
    if (GetAsyncKeyState(VK_NEXT) & 0x8000) { Settings::intensity = std::max(0.f, Settings::intensity - 0.6f * dt); ShowOsd(1.f, Settings::intensity * 0.5f); }
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) { Settings::intensity = std::min(2.f, Settings::intensity + 0.6f * dt); ShowOsd(1.f, Settings::intensity * 0.5f); }
    if (Edge(VK_HOME)) { Settings::intensity = 1.f; ShowOsd(1.f, 0.5f); }

    // F9: presets de FPS simulados (alto -> bajo -> cine)
    static const float presets[] = { 240.f, 120.f, 60.f, 30.f, 24.f };
    static int idx = 0;
    if (Edge(VK_F9)) {
        idx = (idx + 1) % (int)(sizeof(presets) / sizeof(presets[0]));
        Settings::slider = Settings::FpsToSlider(presets[idx]);
        Settings::shutter = presets[idx] <= 30.f ? 0.5f : 1.0f;   // 180° para cine
        ShowOsd(0.f, Settings::slider);
    }
}

// =================================================================== efecto
static void ApplyEffect(IDXGISwapChain* sc)
{
    static bool firstCall = true;
    if (firstCall) { Log("Primer Present recibido: el hook funciona"); firstCall = false; }

    // Tiempo
    static double last = 0.0; static float dtSmooth = 1.f / 60.f;
    double now = NowSec();
    float dt = last > 0.0 ? float(now - last) : 1.f / 60.f;
    last = now;
    bool timeJump = dt > 0.25f;                       // pausa, alt-tab, carga...
    dt = std::clamp(dt, 0.0005f, 0.1f);
    dtSmooth += (dt - dtSmooth) * 0.25f;

    g_osdTimer = std::max(0.f, g_osdTimer - dt);
    PollHotkeys(dt);
    if (!Settings::enabled) { g_r.needReset = true; return; }

    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) {              // no es D3D11 (¿DX12?)
        static bool warned = false;
        if (!warned) { Log("El dispositivo NO es D3D11 (probablemente DX12): el efecto no se puede aplicar"); warned = true; }
        return;
    }

    // Selección de swap chain: nos quedamos con la primera; solo la cambiamos si cambió el dispositivo
    if (g_sc != sc) {
        if (g_sc && g_r.dev && g_r.dev.Get() == dev.Get()) return;  // overlays de terceros, etc.
        g_r.Release(); g_sc = sc;
    }
    if (g_r.dev && g_r.dev.Get() != dev.Get()) g_r.Release();

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    if (bd.SampleDesc.Count > 1 || !(bd.BindFlags & D3D11_BIND_RENDER_TARGET)) return;

    if (!g_r.dev) { g_r.dev = dev; dev->GetImmediateContext(&g_r.ctx); }
    if (!g_r.cur || g_r.w != bd.Width || g_r.h != bd.Height || g_r.fmt != ResolveViewFormat(bd.Format)) {
        ComPtr<ID3D11Device> keep = g_r.dev; ComPtr<ID3D11DeviceContext> keepCtx = g_r.ctx;
        ComPtr<ID3D11VertexShader> vs = g_r.vs; ComPtr<ID3D11PixelShader> ps = g_r.ps;
        // recrear solo texturas, conservar shaders/estados si ya existían
        Renderer fresh; fresh.dev = keep; fresh.ctx = keepCtx;
        fresh.vs = vs; fresh.ps = ps; fresh.cb = g_r.cb; fresh.rs = g_r.rs; fresh.ds = g_r.ds; fresh.bs = g_r.bs;
        g_r = fresh;
        if (!InitResources(g_r, bd)) { Log("ERROR: InitResources fallo (shader o texturas)"); g_r.Release(); g_sc = nullptr; return; }
        Log("Efecto inicializado: %ux%u formato=%d", g_r.w, g_r.h, (int)g_r.fmt);
        ShowOsd(1.f, Settings::intensity * 0.5f);   // barra naranja = confirmación visible
    }
    if (timeJump) g_r.needReset = true;

    ID3D11DeviceContext* ctx = g_r.ctx.Get();

    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = g_r.fmt; rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> bbRtv;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), &rvd, &bbRtv))) return;

    StateBackup sb; sb.Save(ctx);

    // 1) copiar fotograma actual
    ctx->CopyResource(g_r.cur.Get(), bb.Get());

    // 2) constantes
    CBParams p{ dtSmooth, Settings::slider, Settings::shutter, Settings::maxTrail,
                Settings::kMinFps, Settings::kMaxFps, g_r.gammaSpace ? 1.f : 0.f, g_r.needReset ? 1.f : 0.f,
                Settings::intensity, g_osdValue, std::min(1.f, g_osdTimer / 0.5f), g_osdKind };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_r.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(g_r.cb.Get(), 0);
    }

    // 3) pipeline: salida doble -> backbuffer + historial[write]
    const int write = 1 - g_r.readIdx;
    ID3D11RenderTargetView* rts[2] = { bbRtv.Get(), g_r.histRtv[write].Get() };
    ctx->OMSetRenderTargets(2, rts, nullptr);
    ctx->OMSetBlendState(g_r.bs.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_r.ds.Get(), 0);
    ctx->RSSetState(g_r.rs.Get());
    D3D11_VIEWPORT vp{ 0.f, 0.f, (float)g_r.w, (float)g_r.h, 0.f, 1.f };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_r.vs.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_r.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { g_r.curSrv.Get(), g_r.histSrv[g_r.readIdx].Get() };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11Buffer* cbs[1] = { g_r.cb.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ctx->Draw(3, 0);

    // 4) desbindear y restaurar
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ctx->PSSetShaderResources(0, 2, nullSrv);
    ID3D11RenderTargetView* nullRtv[2] = {};
    ctx->OMSetRenderTargets(2, nullRtv, nullptr);
    sb.Restore(ctx);

    g_r.readIdx = write;          // el historial recién escrito es el de lectura del próximo frame
    g_r.needReset = false;
}

// ==================================================================== hooks
static thread_local bool t_busy = false;

static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!t_busy && !(flags & DXGI_PRESENT_TEST)) { t_busy = true; ApplyEffect(sc); t_busy = false; }
    return oPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
    const DXGI_PRESENT_PARAMETERS* pp)
{
    if (!t_busy && !(flags & DXGI_PRESENT_TEST)) { t_busy = true; ApplyEffect(sc); t_busy = false; }
    return oPresent1(sc, sync, flags, pp);
}

static HRESULT STDMETHODCALLTYPE HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    // No debemos tener referencias vivas al backbuffer (nuestras RTV/SRV son propias, el backbuffer no se retiene)
    if (sc == g_sc) g_r.Release();
    return oResize(sc, n, w, h, f, fl);
}

// Obtiene las direcciones reales de la vtable creando una swap chain "dummy"
static bool GetSwapChainVTable(void** vtable, size_t count)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MBDummyWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
        nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> sc; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr);
    if (ok) memcpy(vtable, *reinterpret_cast<void***>(sc.Get()), count * sizeof(void*));

    sc.Reset(); ctx.Reset(); dev.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

static DWORD WINAPI InitThread(LPVOID)
{
    void* vt[23] = {};      // IDXGISwapChain1: Present=8, ResizeBuffers=13, Present1=22
    Log("DLL cargada, buscando vtable de IDXGISwapChain...");
    if (!GetSwapChainVTable(vt, 23)) { Log("ERROR: no se pudo crear la swap chain dummy"); return 1; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) { Log("ERROR: MH_Initialize = %d", (int)st); return 2; }

    MH_STATUS s1 = MH_CreateHook(vt[8], (void*)&HkPresent, (void**)&oPresent);
    MH_STATUS s2 = MH_CreateHook(vt[13], (void*)&HkResize, (void**)&oResize);
    MH_STATUS s3 = MH_CreateHook(vt[22], (void*)&HkPresent1, (void**)&oPresent1);
    MH_STATUS se = MH_EnableHook(MH_ALL_HOOKS);
    Log("Hooks: Present=%d Resize=%d Present1=%d Enable=%d (0 = OK)", (int)s1, (int)s2, (int)s3, (int)se);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Nunca hacer trabajo pesado dentro de DllMain (loader lock): lanzar un hilo.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
