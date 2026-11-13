// Step 0 capture probe. Attaches Windows Graphics Capture to the step 0 test
// window (and only that window class), counts frame arrivals, and optionally
// encodes the frames to MP4 through the Media Foundation sink writer with
// hardware transforms enabled, reporting which encoder was used and CPU cost.
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

static double g_freq = 1.0;
static double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / g_freq;
}
static double cpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    uint64_t kk = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    uint64_t uu = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return double(kk + uu) / 1e7;
}
static void die(const char* what, HRESULT hr) {
    fprintf(stderr, "FAILED %s hr=0x%08lx\n", what, (unsigned long)hr);
    ExitProcess(2);
}
#define OK(hr, what) do { HRESULT _h = (hr); if (FAILED(_h)) die(what, _h); } while (0)

// Monitor capture safety guard: true only while the test window exactly covers
// its monitor and is the window actually showing at five points across it.
static bool targetCoversMonitor(HWND target) {
    if (!IsWindow(target)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT wr;
    GetWindowRect(target, &wr);
    if (!EqualRect(&wr, &mi.rcMonitor)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    POINT pts[5] = {{wr.left + w / 2, wr.top + h / 2}, {wr.left + w / 4, wr.top + h / 4}, {wr.left + 3 * w / 4, wr.top + h / 4},
                    {wr.left + w / 4, wr.top + 3 * h / 4}, {wr.left + 3 * w / 4, wr.top + 3 * h / 4}};
    for (auto& p : pts)
        if (WindowFromPoint(p) != target) return false;
    return true;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring title, out, tag, record;
    double seconds = 8;
    bool monitor = false, dupl = false;
    int sampleX = -1, sampleY = -1;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--title") title = next();
        else if (a == L"--seconds") seconds = _wtof(next().c_str());
        else if (a == L"--out") out = next();
        else if (a == L"--tag") tag = next();
        else if (a == L"--record") record = next();
        else if (a == L"--monitor") monitor = true;
        else if (a == L"--dupl") dupl = true;
        else if (a == L"--sample") { sampleX = _wtoi(next().c_str()); sampleY = _wtoi(next().c_str()); }
    }
    if (monitor && !record.empty()) { fprintf(stderr, "monitor capture is count/sample only in this probe\n"); return 2; }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = double(f.QuadPart);
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    HWND target = nullptr;
    for (int i = 0; i < 50 && !target; i++) {
        target = FindWindowW(L"SnapCapTestWin", title.c_str());
        if (!target) Sleep(100);
    }
    if (!target) { fprintf(stderr, "target window not found\n"); return 2; }

    // D3D11 device shared by capture and encoder
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    OK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                         nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put()), "D3D11CreateDevice");
    d3d.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    if (dupl) {
        // Desktop Duplication of the test window's monitor. Same guard as monitor
        // capture: frames are only counted/sampled while the test window covers the screen.
        if (!targetCoversMonitor(target)) { fprintf(stderr, "refusing duplication: test window does not cover the screen\n"); return 3; }
        HMONITOR hmon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
        winrt::com_ptr<IDXGIAdapter> adapter;
        OK(d3d.as<IDXGIDevice>()->GetAdapter(adapter.put()), "GetAdapter");
        winrt::com_ptr<IDXGIOutput1> output1;
        for (UINT i = 0;; i++) {
            winrt::com_ptr<IDXGIOutput> o;
            if (adapter->EnumOutputs(i, o.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            if (od.Monitor == hmon) { output1 = o.as<IDXGIOutput1>(); break; }
        }
        if (!output1) { fprintf(stderr, "monitor is not on the default adapter\n"); return 4; }
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        OK(d3d->CreateTexture2D(&sd, nullptr, staging.put()), "CreateTexture2D staging");

        winrt::com_ptr<IDXGIOutputDuplication> dup;
        HRESULT firstErr = S_OK;
        int reopen = 0, guardTrips = 0;
        uint64_t frames = 0, accumulated = 0;
        bool sampled = false;
        unsigned char px[4] = {0, 0, 0, 0};
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        Sleep(1000);
        double t0 = now(), c0 = cpuSeconds();
        while (now() - t0 < seconds) {
            if (!dup) {
                HRESULT hr = output1->DuplicateOutput(d3d.get(), dup.put());
                if (FAILED(hr)) { if (SUCCEEDED(firstErr)) firstErr = hr; Sleep(100); continue; }
                reopen++;
                DXGI_OUTDUPL_DESC dd;
                dup->GetDesc(&dd);
                fmt = dd.ModeDesc.Format;
            }
            DXGI_OUTDUPL_FRAME_INFO info{};
            winrt::com_ptr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(100, &info, res.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { if (SUCCEEDED(firstErr)) firstErr = hr; dup = nullptr; continue; }
            if (!targetCoversMonitor(target)) {
                guardTrips++;
            } else if (info.LastPresentTime.QuadPart != 0) {
                frames++;
                accumulated += info.AccumulatedFrames;
                if (!sampled && frames >= 30 && sampleX >= 0 && fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    if (auto tex = res.try_as<ID3D11Texture2D>()) {
                        D3D11_BOX box{UINT(sampleX), UINT(sampleY), 0, UINT(sampleX + 1), UINT(sampleY + 1), 1};
                        ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
                        D3D11_MAPPED_SUBRESOURCE m;
                        if (SUCCEEDED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                            memcpy(px, m.pData, 4);
                            ctx->Unmap(staging.get(), 0);
                            sampled = true;
                        }
                    }
                }
            }
            res = nullptr;
            dup->ReleaseFrame();
        }
        double t1 = now(), c1 = cpuSeconds();
        char line[512];
        snprintf(line, sizeof(line),
                 "{\"kind\":\"capture\",\"tag\":\"%ls\",\"method\":\"duplication\",\"arrival_fps\":%.3f,\"accumulated_fps\":%.3f,\"cpu_core_pct\":%.2f,"
                 "\"format\":%d,\"opens\":%d,\"first_error\":\"0x%08lx\",\"guard_tripped\":%d,\"sampled\":%d,\"sample_rgb\":\"%d,%d,%d\"}\n",
                 tag.c_str(), double(frames) / (t1 - t0), double(accumulated) / (t1 - t0), 100.0 * (c1 - c0) / (t1 - t0),
                 int(fmt), reopen, (unsigned long)firstErr, guardTrips, sampled ? 1 : 0, px[2], px[1], px[0]);
        fputs(line, stdout);
        if (!out.empty()) {
            FILE* fp = _wfopen(out.c_str(), L"ab");
            if (fp) { fputs(line, fp); fclose(fp); }
        }
        return 0;
    }

    winrt::com_ptr<::IInspectable> insp;
    OK(CreateDirect3D11DeviceFromDXGIDevice(d3d.as<IDXGIDevice>().get(), insp.put()), "CreateDirect3D11DeviceFromDXGIDevice");
    auto device = insp.as<wgd::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    if (monitor) {
        if (!targetCoversMonitor(target)) { fprintf(stderr, "refusing monitor capture: test window does not cover the screen\n"); return 3; }
        OK(interop->CreateForMonitor(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)), "CreateForMonitor");
    } else {
        OK(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)), "CreateForWindow");
    }
    auto size = item.Size();
    int encW = size.Width & ~1, encH = size.Height & ~1;

    // "latest frame" texture, written by the capture callback
    D3D11_TEXTURE2D_DESC td{};
    td.Width = size.Width; td.Height = size.Height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> latest;
    OK(d3d->CreateTexture2D(&td, nullptr, latest.put()), "CreateTexture2D latest");

    std::mutex mtx;
    std::atomic<uint64_t> arrivals{0};
    bool haveFrame = false;
    const bool recording = !record.empty();

    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    auto session = pool.CreateCaptureSession(item);
    bool borderOff = false;
    try {
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            session.IsBorderRequired(false);
            borderOff = true;
        }
    } catch (...) {}
    std::atomic<bool> guardTripped{false};
    bool sampled = false;
    unsigned char samplePx[4] = {0, 0, 0, 0};
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (sampleX >= 0) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        OK(d3d->CreateTexture2D(&sd, nullptr, staging.put()), "CreateTexture2D staging");
    }
    pool.FrameArrived([&](wgc::Direct3D11CaptureFramePool const& p, auto&&) {
        auto frame = p.TryGetNextFrame();
        if (!frame) return;
        if (monitor && (guardTripped || !targetCoversMonitor(target))) {
            guardTripped = true; // something else is showing: ignore every frame from here on
            frame.Close();
            return;
        }
        uint64_t count = ++arrivals;
        if (staging && !sampled && count >= 30) {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                std::lock_guard<std::mutex> lk(mtx);
                D3D11_BOX box{UINT(sampleX), UINT(sampleY), 0, UINT(sampleX + 1), UINT(sampleY + 1), 1};
                ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                    memcpy(samplePx, m.pData, 4);
                    ctx->Unmap(staging.get(), 0);
                    sampled = true;
                }
            }
        }
        if (recording) {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                D3D11_TEXTURE2D_DESC fd;
                tex->GetDesc(&fd);
                std::lock_guard<std::mutex> lk(mtx);
                if (fd.Width == td.Width && fd.Height == td.Height) {
                    ctx->CopyResource(latest.get(), tex.get());
                    haveFrame = true;
                }
            }
        }
        frame.Close();
    });
    session.StartCapture();

    // Optional encoder
    winrt::com_ptr<IMFSinkWriter> writer;
    DWORD stream = 0;
    static const int RING = 16;
    winrt::com_ptr<ID3D11Texture2D> ring[RING];
    const int fpsOut = 60;
    if (recording) {
        OK(MFStartup(MF_VERSION), "MFStartup");
        UINT token = 0;
        winrt::com_ptr<IMFDXGIDeviceManager> mgr;
        OK(MFCreateDXGIDeviceManager(&token, mgr.put()), "MFCreateDXGIDeviceManager");
        OK(mgr->ResetDevice(d3d.get(), token), "ResetDevice");
        winrt::com_ptr<IMFAttributes> attrs;
        OK(MFCreateAttributes(attrs.put(), 4), "MFCreateAttributes");
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr.get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        DeleteFileW(record.c_str());
        OK(MFCreateSinkWriterFromURL(record.c_str(), nullptr, attrs.get(), writer.put()), "MFCreateSinkWriterFromURL");
        winrt::com_ptr<IMFMediaType> ot, it;
        OK(MFCreateMediaType(ot.put()), "MFCreateMediaType");
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, 12000000);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(ot.get(), MF_MT_FRAME_SIZE, encW, encH);
        MFSetAttributeRatio(ot.get(), MF_MT_FRAME_RATE, fpsOut, 1);
        MFSetAttributeRatio(ot.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        OK(writer->AddStream(ot.get(), &stream), "AddStream");
        OK(MFCreateMediaType(it.put()), "MFCreateMediaType");
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.get(), MF_MT_FRAME_SIZE, encW, encH);
        MFSetAttributeRatio(it.get(), MF_MT_FRAME_RATE, fpsOut, 1);
        MFSetAttributeRatio(it.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        OK(writer->SetInputMediaType(stream, it.get(), nullptr), "SetInputMediaType");
        OK(writer->BeginWriting(), "BeginWriting");
        D3D11_TEXTURE2D_DESC rd = td;
        rd.Width = encW; rd.Height = encH;
        for (auto& t : ring) OK(d3d->CreateTexture2D(&rd, nullptr, t.put()), "CreateTexture2D ring");
    }

    // Measure
    const double warm = 1.0;
    Sleep(DWORD(warm * 1000));
    uint64_t a0 = arrivals.load();
    double t0 = now(), c0 = cpuSeconds();
    int written = 0;
    HRESULT writeErr = S_OK;
    if (recording) {
        int total = int(seconds * fpsOut);
        const LONGLONG dur = 10000000LL / fpsOut;
        for (int i = 0; i < total; i++) {
            double due = t0 + double(i) / fpsOut, rem;
            while ((rem = due - now()) > 0) { if (rem > 0.002) Sleep(1); else YieldProcessor(); }
            ID3D11Texture2D* dst = ring[i % RING].get();
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!haveFrame) continue;
                D3D11_BOX box{0, 0, 0, UINT(encW), UINT(encH), 1};
                ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, latest.get(), 0, &box);
            }
            winrt::com_ptr<IMFMediaBuffer> buf;
            winrt::com_ptr<IMFSample> sample;
            HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), dst, 0, FALSE, buf.put());
            if (SUCCEEDED(hr)) {
                DWORD len = 0;
                if (auto b2 = buf.try_as<IMF2DBuffer>()) b2->GetContiguousLength(&len);
                buf->SetCurrentLength(len);
                hr = MFCreateSample(sample.put());
            }
            if (SUCCEEDED(hr)) {
                sample->AddBuffer(buf.get());
                sample->SetSampleTime(LONGLONG(i) * dur);
                sample->SetSampleDuration(dur);
                hr = writer->WriteSample(stream, sample.get());
            }
            if (FAILED(hr)) { writeErr = hr; break; }
            written++;
        }
    } else {
        Sleep(DWORD(seconds * 1000));
    }
    double t1 = now(), c1 = cpuSeconds();
    uint64_t a1 = arrivals.load();

    // Which transforms did the sink writer build?
    std::string transforms;
    bool hwEncoder = false;
    if (recording) {
        if (auto ex = writer.try_as<IMFSinkWriterEx>()) {
            for (DWORD idx = 0;; idx++) {
                GUID cat{};
                winrt::com_ptr<IMFTransform> mft;
                if (FAILED(ex->GetTransformForStream(stream, idx, &cat, mft.put()))) break;
                const char* kind = cat == MFT_CATEGORY_VIDEO_ENCODER ? "encoder" : cat == MFT_CATEGORY_VIDEO_PROCESSOR ? "processor" : "other";
                bool hw = false, d3dAware = false;
                winrt::com_ptr<IMFAttributes> ma;
                if (SUCCEEDED(mft->GetAttributes(ma.put())) && ma) {
                    UINT32 n = 0;
                    hw = SUCCEEDED(ma->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &n));
                    UINT32 v = 0;
                    d3dAware = SUCCEEDED(ma->GetUINT32(MF_SA_D3D11_AWARE, &v)) && v;
                }
                if (cat == MFT_CATEGORY_VIDEO_ENCODER && hw) hwEncoder = true;
                char b[96];
                snprintf(b, sizeof(b), "%s%s(hw=%d,d3d11=%d)", transforms.empty() ? "" : " ", kind, hw ? 1 : 0, d3dAware ? 1 : 0);
                transforms += b;
            }
        }
        HRESULT fin = writer->Finalize();
        if (FAILED(fin) && SUCCEEDED(writeErr)) writeErr = fin;
    }
    session.Close();
    pool.Close();

    char line[640];
    snprintf(line, sizeof(line),
             "{\"kind\":\"capture\",\"tag\":\"%ls\",\"w\":%d,\"h\":%d,\"border_off\":%d,\"arrival_fps\":%.3f,\"cpu_core_pct\":%.2f,"
             "\"recording\":%d,\"frames_written\":%d,\"write_hr\":\"0x%08lx\",\"hw_encoder\":%d,\"transforms\":\"%s\","
             "\"monitor\":%d,\"guard_tripped\":%d,\"sampled\":%d,\"sample_rgb\":\"%d,%d,%d\"}\n",
             tag.c_str(), size.Width, size.Height, borderOff ? 1 : 0, double(a1 - a0) / (t1 - t0), 100.0 * (c1 - c0) / (t1 - t0),
             recording ? 1 : 0, written, (unsigned long)writeErr, hwEncoder ? 1 : 0, transforms.c_str(),
             monitor ? 1 : 0, guardTripped.load() ? 1 : 0, sampled ? 1 : 0, samplePx[2], samplePx[1], samplePx[0]);
    fputs(line, stdout);
    if (!out.empty()) {
        FILE* fp = _wfopen(out.c_str(), L"ab");
        if (fp) { fputs(line, fp); fclose(fp); }
    }
    if (recording) MFShutdown();
    timeEndPeriod(1);
    return 0;
}
