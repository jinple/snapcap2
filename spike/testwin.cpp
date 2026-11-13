// Step 0 test window: presents at a known rate with DX11, DX12 or OpenGL and
// reports its own exact frame count. Everything else is measured against this.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <cmath>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <gl/GL.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

static double g_freq = 1.0;
static double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / g_freq;
}
static double unixNow() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return double(t) / 1e7 - 11644473600.0;
}
#define CHECK(hr, what) do { HRESULT _h = (hr); if (FAILED(_h)) { fprintf(stderr, "FAILED %s hr=0x%08lx\n", what, (unsigned long)_h); return false; } } while (0)

struct Renderer {
    virtual bool init(HWND hwnd, int w, int h, int load) = 0;
    virtual void frame(uint64_t n) = 0;
    virtual ~Renderer() {}
};

// No flashing: the picture is a steady dark grey. The only thing that changes
// is a small, dim square drifting slowly along the top edge, so that capture
// still sees new content.
static void colourFor(uint64_t, float c[4]) {
    c[0] = 0.10f; c[1] = 0.11f; c[2] = 0.13f; c[3] = 1.0f;
}
static const float kMarker[4] = {0.16f, 0.17f, 0.19f, 1.0f};
static const int kMarkerSize = 12;
static int markerX(int w) {
    int span = w - 16 - kMarkerSize;
    if (span < 1) return 0;
    return 8 + int(fmod(now() * 40.0, double(span)));
}

// ---------------------------------------------------------------- DX11
struct Dx11 : Renderer {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    int w = 0, h = 0, load = 0;

    bool init(HWND hwnd, int w_, int h_, int load_) override {
        w = w_; h = h_; load = load_;
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
        ctx.As(&ctx1);
        ComPtr<IDXGIFactory2> fac;
        CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        CHECK(fac->CreateSwapChainForHwnd(dev.Get(), hwnd, &d, nullptr, nullptr, &sc), "CreateSwapChainForHwnd");
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> bb;
        CHECK(sc->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
        CHECK(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv), "CreateRTV");

        static const char* vsSrc =
            "float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);"
            "return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        static const char* psSrc =
            "cbuffer C:register(b0){float4 col;int iters;int3 pad;};"
            "float4 main(float4 pos:SV_Position):SV_Target{float a=0;"
            "for(int i=0;i<iters;i++){a+=sin(pos.x*0.01+i*0.37+a)*cos(pos.y*0.01+a*0.11);}"
            "return float4(col.rgb*(0.9+0.1*frac(a)),1);}";
        ComPtr<ID3DBlob> vb, pb, err;
        CHECK(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &err), "compile vs");
        CHECK(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err), "compile ps");
        CHECK(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "CreateVS");
        CHECK(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps), "CreatePS");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CHECK(dev->CreateBuffer(&bd, nullptr, &cb), "CreateBuffer");
        return true;
    }
    void frame(uint64_t n) override {
        float c[4];
        colourFor(n, c);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), c);
        if (load > 0) {
            struct { float col[4]; int iters; int pad[3]; } data{};
            memcpy(data.col, c, sizeof(c));
            data.iters = load;
            ctx->UpdateSubresource(cb.Get(), 0, nullptr, &data, 0, 0);
            D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
            ctx->Draw(3, 0);
        }
        if (ctx1) {
            int x = markerX(w);
            D3D11_RECT mr{x, 8, x + kMarkerSize, 8 + kMarkerSize};
            ctx1->ClearView(rtv.Get(), kMarker, &mr, 1);
        }
        sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    }
};

// ---------------------------------------------------------------- DX12
struct Dx12 : Renderer {
    static const int N = 2;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> bb[N];
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    uint64_t fv = 0;
    UINT rtvSize = 0;
    int w = 0;

    bool init(HWND hwnd, int w_, int h, int) override {
        w = w_;
        CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "CreateCommandQueue");
        ComPtr<IDXGIFactory2> fac;
        CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = N; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        ComPtr<IDXGISwapChain1> sc1;
        CHECK(fac->CreateSwapChainForHwnd(q.Get(), hwnd, &d, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        CHECK(sc1.As(&sc), "IDXGISwapChain3");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = N;
        CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
        for (int i = 0; i < N; i++) {
            CHECK(sc->GetBuffer(i, IID_PPV_ARGS(&bb[i])), "GetBuffer");
            dev->CreateRenderTargetView(bb[i].Get(), nullptr, hnd);
            hnd.ptr += rtvSize;
        }
        CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "CreateCommandAllocator");
        CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCommandList");
        cl->Close();
        CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void frame(uint64_t n) override {
        float c[4];
        colourFor(n, c);
        UINT i = sc->GetCurrentBackBufferIndex();
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb[i].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap->GetCPUDescriptorHandleForHeapStart();
        hnd.ptr += SIZE_T(i) * rtvSize;
        cl->ClearRenderTargetView(hnd, c, 0, nullptr);
        int mx = markerX(w);
        D3D12_RECT mr{mx, 8, mx + kMarkerSize, 8 + kMarkerSize};
        cl->ClearRenderTargetView(hnd, kMarker, 1, &mr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        q->ExecuteCommandLists(1, lists);
        sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        q->Signal(fence.Get(), ++fv);
        if (fence->GetCompletedValue() < fv) {
            fence->SetEventOnCompletion(fv, ev);
            WaitForSingleObject(ev, INFINITE);
        }
    }
};

// ---------------------------------------------------------------- OpenGL
struct Gl : Renderer {
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    int w = 0, h = 0;
    bool init(HWND hwnd, int w_, int h_, int) override {
        w = w_; h = h_;
        dc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
        int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) { fprintf(stderr, "FAILED SetPixelFormat\n"); return false; }
        rc = wglCreateContext(dc);
        if (!rc || !wglMakeCurrent(dc, rc)) { fprintf(stderr, "FAILED wglCreateContext\n"); return false; }
        typedef BOOL(WINAPI * SwapIntervalFn)(int);
        SwapIntervalFn swapInterval = (SwapIntervalFn)wglGetProcAddress("wglSwapIntervalEXT");
        if (swapInterval) swapInterval(0);
        printf("gl renderer: %s\n", (const char*)glGetString(GL_RENDERER));
        return true;
    }
    void frame(uint64_t n) override {
        float c[4];
        colourFor(n, c);
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(markerX(w), h - 8 - kMarkerSize, kMarkerSize, kMarkerSize);
        glClearColor(kMarker[0], kMarker[1], kMarker[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        SwapBuffers(dc);
    }
};

// ---------------------------------------------------------------- Vulkan
// Loaded at run time from vulkan-1.dll (installed with the graphics driver).
// No render pass or shaders: clear the swapchain image, copy in the marker.
#define VK_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkCreateWin32SurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkCreateSemaphore) X(vkCreateFence) X(vkCreateImage) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) X(vkAcquireNextImageKHR) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkCmdCopyImage) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkWaitForFences) \
    X(vkResetFences)
#define VK_DECL(n) static PFN_##n n = nullptr;
VK_FUNCS(VK_DECL)
#define VKCHECK(r, what) do { VkResult _r = (r); if (_r != VK_SUCCESS) { fprintf(stderr, "FAILED %s vk=%d\n", what, (int)_r); return false; } } while (0)

struct Vk : Renderer {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkImage images[8] = {};
    uint32_t imageCount = 0;
    VkImage marker = VK_NULL_HANDLE;
    bool markerReady = false;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkSemaphore semAcquire = VK_NULL_HANDLE, semRender = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkExtent2D extent{};

    static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool init(HWND hwnd, int, int, int) override {
        HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
        if (!lib) { fprintf(stderr, "FAILED to load vulkan-1.dll\n"); return false; }
        auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
        vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "snapcap-testwin";
        ai.apiVersion = VK_API_VERSION_1_0;
        const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
        VKCHECK(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");
#define VK_LOAD(n) n = (PFN_##n)gipa(inst, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); return false; }
        VK_FUNCS(VK_LOAD)

        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr); sci.hwnd = hwnd;
        VKCHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surf), "vkCreateWin32SurfaceKHR");

        VkPhysicalDevice pds[8];
        uint32_t n = 8;
        vkEnumeratePhysicalDevices(inst, &n, pds);
        uint32_t qf = 0;
        int bestScore = -1;
        for (uint32_t i = 0; i < n; i++) {
            VkQueueFamilyProperties qp[16];
            uint32_t qn = 16;
            vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qp);
            for (uint32_t q = 0; q < qn; q++) {
                VkBool32 ok = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &ok);
                if (!ok || !(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkPhysicalDeviceProperties pp;
                vkGetPhysicalDeviceProperties(pds[i], &pp);
                int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore) { bestScore = score; pd = pds[i]; qf = q; printf("vk device: %s\n", pp.deviceName); }
                break;
            }
        }
        if (!pd) { fprintf(stderr, "no Vulkan device can present to the window\n"); return false; }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        const char* dext[] = {"VK_KHR_swapchain"};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
        VKCHECK(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice");
        vkGetDeviceQueue(dev, qf, 0, &queue);

        VkSurfaceCapabilitiesKHR caps;
        VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps), "surface caps");
        VkSurfaceFormatKHR fmts[64];
        uint32_t fn = 64;
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fn, fmts);
        VkSurfaceFormatKHR fmt = fmts[0];
        for (uint32_t i = 0; i < fn; i++)
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = fmts[i]; break; }
        VkPresentModeKHR modes[16];
        uint32_t mn = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &mn, modes);
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (uint32_t i = 0; i < mn; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        for (uint32_t i = 0; i < mn; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
        printf("vk present mode: %d (0=immediate 1=mailbox 2=fifo)\n", (int)mode);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "swapchain cannot be a transfer target\n"); return false; }
        extent = caps.currentExtent;
        VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sw.surface = surf;
        sw.minImageCount = caps.minImageCount + 1;
        if (caps.maxImageCount && sw.minImageCount > caps.maxImageCount) sw.minImageCount = caps.maxImageCount;
        sw.imageFormat = fmt.format; sw.imageColorSpace = fmt.colorSpace;
        sw.imageExtent = extent; sw.imageArrayLayers = 1;
        sw.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sw.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sw.preTransform = caps.currentTransform;
        sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sw.presentMode = mode; sw.clipped = VK_TRUE;
        VKCHECK(vkCreateSwapchainKHR(dev, &sw, nullptr, &sc), "vkCreateSwapchainKHR");
        imageCount = 8;
        vkGetSwapchainImagesKHR(dev, sc, &imageCount, images);

        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt.format;
        ii.extent = {uint32_t(kMarkerSize), uint32_t(kMarkerSize), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKCHECK(vkCreateImage(dev, &ii, nullptr, &marker), "vkCreateImage");
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, marker, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        uint32_t mt = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
        if (mt == UINT32_MAX) { fprintf(stderr, "no memory type for marker image\n"); return false; }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        VkDeviceMemory mem;
        VKCHECK(vkAllocateMemory(dev, &mai, nullptr, &mem), "vkAllocateMemory");
        VKCHECK(vkBindImageMemory(dev, marker, mem, 0), "vkBindImageMemory");

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = qf;
        VkCommandPool pool;
        VKCHECK(vkCreateCommandPool(dev, &cpi, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(dev, &cai, &cb), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(dev, &si, nullptr, &semAcquire), "vkCreateSemaphore");
        VKCHECK(vkCreateSemaphore(dev, &si, nullptr, &semRender), "vkCreateSemaphore");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKCHECK(vkCreateFence(dev, &fi, nullptr, &fence), "vkCreateFence");
        return true;
    }
    void frame(uint64_t n) override {
        float c[4];
        colourFor(n, c);
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, semAcquire, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "acquire failed vk=%d\n", (int)ar); ExitProcess(5); }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!markerReady) {
            barrier(cb, marker, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue mc{};
            memcpy(mc.float32, kMarker, sizeof(float) * 4);
            vkCmdClearColorImage(cb, marker, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mc, 1, &range);
            barrier(cb, marker, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            markerReady = true;
        }
        barrier(cb, images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cc{};
        memcpy(cc.float32, c, sizeof(float) * 4);
        vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
        if (extent.width > uint32_t(kMarkerSize + 32) && extent.height > uint32_t(kMarkerSize + 16)) {
            VkImageCopy cp{};
            cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.dstOffset = {markerX(int(extent.width)), 8, 0};
            cp.extent = {uint32_t(kMarkerSize), uint32_t(kMarkerSize), 1};
            vkCmdCopyImage(cb, marker, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        }
        barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &semAcquire; sub.pWaitDstStageMask = &wait;
        sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
        sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &semRender;
        vkQueueSubmit(queue, 1, &sub, fence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semRender;
        pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        vkQueuePresentKHR(queue, &pi);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
    }
};

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// Plays a quiet, steady sine tone from this process until it exits (for the
// application-only audio test). One second of sound, looped.
static void startTone(int hz) {
    static std::string wav;
    const int rate = 48000, n = rate;
    struct { char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmtSize; uint16_t tag, ch; uint32_t rate, bytes; uint16_t align, bits; char data[4]; uint32_t dataSize; } h =
        {{'R','I','F','F'}, 36 + n * 4, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, 2, rate, rate * 4, 4, 16, {'d','a','t','a'}, uint32_t(n * 4)};
    wav.assign((const char*)&h, sizeof(h));
    for (int i = 0; i < n; i++) {
        int16_t v = int16_t(0.08 * 32767.0 * sin(2.0 * 3.14159265358979 * hz * i / rate));
        wav.append((const char*)&v, 2);
        wav.append((const char*)&v, 2);
    }
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring api = L"dx11", out, tag;
    double fps = 60, seconds = 10;
    int w = 1280, h = 720, load = 0;
    bool borderless = false, toneOn = false;
    int tone = 0;
    double toneAt = 0, toneStop = 0;
    bool toneOff = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--api") api = next();
        else if (a == L"--fps") fps = _wtof(next().c_str());
        else if (a == L"--seconds") seconds = _wtof(next().c_str());
        else if (a == L"--w") w = _wtoi(next().c_str());
        else if (a == L"--h") h = _wtoi(next().c_str());
        else if (a == L"--load") load = _wtoi(next().c_str());
        else if (a == L"--out") out = next();
        else if (a == L"--tag") tag = next();
        else if (a == L"--borderless") borderless = true;
        else if (a == L"--tone") tone = _wtoi(next().c_str());       // quiet sine tone at this many Hz
        else if (a == L"--tone-at") toneAt = _wtof(next().c_str());  // seconds after start
        else if (a == L"--tone-stop") toneStop = _wtof(next().c_str());
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = double(f.QuadPart);
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnapCapTestWin";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    std::wstring title = L"snapcap-testwin-" + api;
    HWND hwnd;
    if (borderless) {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
        hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_POPUP, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    } else {
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 80, 80, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    Renderer* r = nullptr;
    if (api == L"dx11") r = new Dx11();
    else if (api == L"dx12") r = new Dx12();
    else if (api == L"gl") r = new Gl();
    else if (api == L"vk") r = new Vk();
    if (!r || !r->init(hwnd, w, h, load)) { fprintf(stderr, "init failed for %ls\n", api.c_str()); return 2; }

    const double warm = 2.0, tail = 1.0;
    double t0 = now(), nextT = t0;
    uint64_t n = 0, f0 = 0, f1 = 0;
    double m0 = 0, m1 = 0, u0 = 0, u1 = 0;
    bool started = false, ended = false, quit = false;
    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            DispatchMessageW(&msg);
        }
        if (fps > 0) {
            double rem;
            while ((rem = nextT - now()) > 0) {
                if (rem > 0.002) Sleep(1); else YieldProcessor();
            }
            nextT += 1.0 / fps;
            if (now() - nextT > 0.1) nextT = now();
        }
        r->frame(n++);
        double t = now() - t0;
        if (tone > 0 && !toneOn && t >= toneAt) {
            toneOn = true;
            startTone(tone);
            if (!out.empty()) {
                if (FILE* fp = _wfopen(out.c_str(), L"ab")) { fprintf(fp, "{\"kind\":\"tone\",\"hz\":%d,\"t\":%.3f}\n", tone, unixNow()); fclose(fp); }
            }
        }
        if (toneOn && !toneOff && toneStop > 0 && t >= toneStop) {
            toneOff = true;
            PlaySoundW(nullptr, nullptr, 0);
            if (!out.empty()) {
                if (FILE* fp = _wfopen(out.c_str(), L"ab")) { fprintf(fp, "{\"kind\":\"tone_stop\",\"t\":%.3f}\n", unixNow()); fclose(fp); }
            }
        }
        if (!started && t >= warm) { started = true; m0 = t; f0 = n; u0 = unixNow(); }
        if (started && !ended && t >= seconds - tail) { ended = true; m1 = t; f1 = n; u1 = unixNow(); }
        if (t >= seconds) break;
    }
    double measured = (ended && m1 > m0) ? double(f1 - f0) / (m1 - m0) : 0;
    char line[512];
    snprintf(line, sizeof(line),
             "{\"kind\":\"truth\",\"tag\":\"%ls\",\"pid\":%lu,\"api\":\"%ls\",\"borderless\":%d,\"w\":%d,\"h\":%d,\"target\":%.3f,\"load\":%d,\"t0\":%.3f,\"t1\":%.3f,\"frames\":%llu,\"fps\":%.3f}\n",
             tag.c_str(), GetCurrentProcessId(), api.c_str(), borderless ? 1 : 0, w, h, fps, load, u0, u1,
             (unsigned long long)(f1 - f0), measured);
    fputs(line, stdout);
    if (!out.empty()) {
        FILE* fp = _wfopen(out.c_str(), L"ab");
        if (fp) { fputs(line, fp); fclose(fp); }
    }
    timeEndPeriod(1);
    return 0;
}
