// See recorder.h. Everything here runs on threads the recorder owns, so the
// tray and overlay never wait on capture, encoding or file writing.
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "recorder.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
using winrt::com_ptr;
using winrt::check_hresult;

namespace rec {
namespace {

int kFps = 60; // frames a second of the video; set from the options when a recording starts
const int kAudioRate = 48000, kAudioBlock = 4; // 16-bit stereo
const int kRing = 16;

struct Params { HWND target = nullptr; DWORD pid = 0; std::wstring name; HWND notify = nullptr; UINT msg = 0; };
struct Fail { std::wstring msg; };

std::atomic<int> g_state{Idle};
HANDLE g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
HANDLE g_thread = nullptr;
Params g_params;
std::mutex g_textMx;
std::wstring g_textName, g_textFps;
std::atomic<bool> g_showInVideo{true};
std::mutex g_optsMx;
Options g_opts;
std::mutex g_resultMx;
Result g_result;
SRWLOCK g_arrLock = SRWLOCK_INIT;
std::deque<LONGLONG> g_arrivals;

LONGLONG qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
LONGLONG qpf() { static LONGLONG f = [] { LARGE_INTEGER c; QueryPerformanceFrequency(&c); return c.QuadPart; }(); return f; }
LONGLONG ticksTo100ns(LONGLONG t) { return (t / qpf()) * 10000000 + (t % qpf()) * 10000000 / qpf(); }
bool stopRequested() { return WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0; }

void noteArrival() {
    LONGLONG t = qpc();
    AcquireSRWLockExclusive(&g_arrLock);
    g_arrivals.push_back(t);
    while (!g_arrivals.empty() && g_arrivals.front() < t - qpf()) g_arrivals.pop_front();
    ReleaseSRWLockExclusive(&g_arrLock);
}

// True only while the window exactly covers its monitor and is what shows at
// five points across it. Whole-screen capture is used only while this holds.
bool coversMonitor(HWND target) {
    if (!IsWindow(target)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT wr;
    GetWindowRect(target, &wr);
    if (!EqualRect(&wr, &mi.rcMonitor)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    POINT pts[5] = {{wr.left + w / 2, wr.top + h / 2}, {wr.left + w / 4, wr.top + h / 4}, {wr.left + 3 * w / 4, wr.top + h / 4},
                    {wr.left + w / 4, wr.top + 3 * h / 4}, {wr.left + 3 * w / 4, wr.top + 3 * h / 4}};
    for (auto& p : pts) {
        HWND at = WindowFromPoint(p);
        if (!at || GetAncestor(at, GA_ROOT) != target) return false;
    }
    return true;
}

struct AudioActivation : winrt::implements<AudioActivation, IActivateAudioInterfaceCompletionHandler> {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    com_ptr<IAudioClient> client;
    HRESULT hr = E_FAIL;
    ~AudioActivation() { CloseHandle(done); }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) noexcept override {
        HRESULT activated = E_FAIL;
        com_ptr<::IUnknown> unk;
        hr = op->GetActivateResult(&activated, unk.put());
        if (SUCCEEDED(hr)) hr = activated;
        if (SUCCEEDED(hr) && unk) client = unk.try_as<IAudioClient>();
        SetEvent(done);
        return S_OK;
    }
};

struct Slot { // one frame on its way to the encoder, already in the encoder's own format (NV12)
    com_ptr<ID3D11Texture2D> tex;
    com_ptr<ID3D11VideoProcessorOutputView> out;
    com_ptr<IMFMediaBuffer> buf;
    bool free() { buf->AddRef(); return buf->Release() == 1; } // nobody downstream still holds it
};

struct Session {
    Params p;
    Options opt;
    Result r;
    const wchar_t* stage = L"starting";

    com_ptr<ID3D11Device> d3d;
    com_ptr<ID3D11DeviceContext> ctx;
    com_ptr<ID2D1Factory1> d2dFactory;
    com_ptr<ID2D1Multithread> d2dLock;
    com_ptr<ID2D1Device> d2dDevice;
    com_ptr<ID2D1DeviceContext> dc;
    com_ptr<IDWriteFactory> dw;
    com_ptr<IDWriteTextFormat> textFormat;
    com_ptr<ID2D1SolidColorBrush> brBox, brName, brFps;
    float fontPx = 20;

    wgd::Direct3D11::IDirect3DDevice wdevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    std::mutex frameMx;
    // The newest picture. From window capture it is the capture's own texture, kept
    // alive by holding its frame (no copy). From whole-screen capture it is our copy.
    wgc::Direct3D11CaptureFrame heldFrame{nullptr};
    com_ptr<ID3D11Texture2D> latest;
    bool latestOwned = false;
    std::vector<std::pair<ID3D11Texture2D*, com_ptr<ID2D1Bitmap1>>> bmpCache; // Direct2D views of capture textures
    bool haveFrame = false;

    // Drawing and conversion for the encoder, all queued to the GPU in one go per frame.
    com_ptr<ID3D11VideoDevice> vdev;
    com_ptr<ID3D11VideoContext> vctx;
    com_ptr<ID3D11VideoProcessorEnumerator> vpEnum;
    com_ptr<ID3D11VideoProcessor> vproc;
    com_ptr<ID3D11Texture2D> canvas;                 // BGRA, video size: picture + text are drawn here
    com_ptr<ID2D1Bitmap1> canvasBmp;
    com_ptr<ID3D11VideoProcessorInputView> canvasIn;
    std::atomic<bool> screen{false};            // whole-screen capture is the picture source
    std::atomic<int> wgcCount{0}, duplCount{0}; // frames each method has delivered
    HANDLE duplThread = nullptr;
    std::atomic<bool> duplStop{false}, duplOpen{false};
    int wPrev = 0, dPrev = 0;
    D2D1_RECT_F crop{};
    int cropAge = 1000;
    UINT lastW = 0, lastH = 0;

    int encW = 0, encH = 0;
    com_ptr<IMFSinkWriter> writer;
    std::mutex writerMx;
    DWORD vStream = 0, aStream = 0;
    bool hasAudio = false;
    Slot ring[kRing];
    LONGLONG t0 = 0;
    std::atomic<bool> clockStarted{false};
    std::atomic<LONGLONG> end100ns{-1};

    com_ptr<IAudioClient> audioClient;
    com_ptr<IAudioCaptureClient> audioCapture;
    HANDLE audioEvent = nullptr, audioThread = nullptr;
    std::atomic<bool> audioStop{false};

    // ---------------------------------------------------------- devices
    void createDevices() {
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put()));
        d3d.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
        vdev = d3d.as<ID3D11VideoDevice>();
        vctx = ctx.as<ID3D11VideoContext>();
        D2D1_FACTORY_OPTIONS fo{};
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, d2dFactory.put_void()));
        d2dLock = d2dFactory.as<ID2D1Multithread>();
        check_hresult(d2dFactory->CreateDevice(d3d.as<IDXGIDevice>().get(), d2dDevice.put()));
        check_hresult(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown**>(dw.put())));
        com_ptr<::IInspectable> insp;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(d3d.as<IDXGIDevice>().get(), insp.put()));
        wdevice = insp.as<wgd::Direct3D11::IDirect3DDevice>();
    }

    // Keeps a copy of the newest whole-screen picture (those frames cannot be held).
    void copyLatest(ID3D11Texture2D* src) {
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        std::lock_guard<std::mutex> lk(frameMx);
        D3D11_TEXTURE2D_DESC ld{};
        if (latest) latest->GetDesc(&ld);
        if (!latest || !latestOwned || ld.Width != sd.Width || ld.Height != sd.Height || ld.Format != sd.Format) {
            releaseHeld();
            latestOwned = true;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width; td.Height = sd.Height; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = sd.Format; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d->CreateTexture2D(&td, nullptr, latest.put()))) return;
            cropAge = 1000;
        }
        d2dLock->Enter();
        ctx->CopyResource(latest.get(), src);
        d2dLock->Leave();
        haveFrame = true;
    }

    void releaseHeld() { // frameMx must be held
        bmpCache.clear();
        latest = nullptr;
        latestOwned = false;
        if (heldFrame) { try { heldFrame.Close(); } catch (...) {} heldFrame = nullptr; }
    }

    // ---------------------------------------------------------- window capture
    void startWindowCapture() {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(p.target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        poolSize = item.Size();
        const auto fmt = wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(wdevice, fmt, 3, poolSize);
        session = pool.CreateCaptureSession(item);
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
                session.IsBorderRequired(false);
        } catch (...) {}
        pool.FrameArrived([this, fmt](wgc::Direct3D11CaptureFramePool const& sender, auto&&) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            auto cs = frame.ContentSize();
            bool resized = cs.Width != poolSize.Width || cs.Height != poolSize.Height;
            bool kept = false;
            if (!resized && !screen.load()) {
                // Keep this frame as the newest picture and let the previous one go.
                // No GPU work happens here; the frame is only read when a video frame is drawn.
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                com_ptr<ID3D11Texture2D> tex;
                if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                    wgc::Direct3D11CaptureFrame old{nullptr};
                    {
                        std::lock_guard<std::mutex> lk(frameMx);
                        if (!screen.load()) {
                            old = heldFrame;
                            heldFrame = frame;
                            latest = tex;
                            latestOwned = false;
                            haveFrame = true;
                            kept = true;
                        }
                    }
                    if (old) { try { old.Close(); } catch (...) {} }
                }
            }
            if (!kept) frame.Close();
            if (resized && cs.Width > 0 && cs.Height > 0) { // the window changed size: capture at the new size
                poolSize = cs;
                { std::lock_guard<std::mutex> lk(frameMx); if (!latestOwned) releaseHeld(); }
                try { sender.Recreate(wdevice, fmt, 3, cs); } catch (...) {}
            }
            wgcCount++;
            if (!screen.load()) noteArrival();
        });
        session.StartCapture();
    }

    // ---------------------------------------------------------- whole-screen capture
    // Used only for a fullscreen window that window capture cannot see. Copies a
    // picture only while that window covers the whole monitor.
    static DWORD WINAPI duplThunk(LPVOID s) { ((Session*)s)->duplLoop(); return 0; }
    void duplLoop() {
        HMONITOR mon = MonitorFromWindow(p.target, MONITOR_DEFAULTTONEAREST);
        com_ptr<IDXGIAdapter> adapter;
        if (FAILED(d3d.as<IDXGIDevice>()->GetAdapter(adapter.put()))) return;
        com_ptr<IDXGIOutput1> output;
        for (UINT i = 0;; i++) {
            com_ptr<IDXGIOutput> o;
            if (adapter->EnumOutputs(i, o.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            if (od.Monitor == mon) { output = o.try_as<IDXGIOutput1>(); break; }
        }
        if (!output) return;
        com_ptr<IDXGIOutputDuplication> dup;
        LONGLONG lastCopy = 0;
        while (!duplStop) {
            if (!dup) {
                if (FAILED(output->DuplicateOutput(d3d.get(), dup.put()))) { dup = nullptr; Sleep(100); continue; }
                duplOpen = true;
            }
            DXGI_OUTDUPL_FRAME_INFO info{};
            com_ptr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(100, &info, res.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { dup = nullptr; Sleep(50); continue; }
            if (info.LastPresentTime.QuadPart != 0) duplCount++; // counting only; no picture is touched
            if (info.LastPresentTime.QuadPart != 0 && screen.load()) {
                noteArrival();
                LONGLONG now = qpc();
                if (now - lastCopy > qpf() / 250 && coversMonitor(p.target)) { // at most 250 copies a second
                    if (auto tex = res.try_as<ID3D11Texture2D>()) copyLatest(tex.get());
                    lastCopy = now;
                }
            }
            res = nullptr;
            dup->ReleaseFrame();
        }
    }

    bool waitForFrame(int ms) {
        for (int waited = 0; waited < ms; waited += 20) {
            { std::lock_guard<std::mutex> lk(frameMx); if (haveFrame) return true; }
            if (WaitForSingleObject(g_stop, 20) == WAIT_OBJECT_0) return false;
        }
        std::lock_guard<std::mutex> lk(frameMx);
        return haveFrame;
    }

    // ---------------------------------------------------------- audio
    bool initAudio() {
        try {
            AUDIOCLIENT_ACTIVATION_PARAMS ap{};
            ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            ap.ProcessLoopbackParams.TargetProcessId = p.pid;
            ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            PROPVARIANT pv{};
            pv.vt = VT_BLOB;
            pv.blob.cbSize = sizeof(ap);
            pv.blob.pBlobData = (BYTE*)&ap;
            auto handler = winrt::make_self<AudioActivation>();
            com_ptr<IActivateAudioInterfaceAsyncOperation> op;
            check_hresult(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.get(), op.put()));
            if (WaitForSingleObject(handler->done, 3000) != WAIT_OBJECT_0) return false;
            if (FAILED(handler->hr) || !handler->client) return false;
            audioClient = handler->client;
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = kAudioRate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = kAudioBlock;
            wf.nAvgBytesPerSec = kAudioRate * kAudioBlock;
            check_hresult(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                  AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                                  2000000, 0, &wf, nullptr));
            audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check_hresult(audioClient->SetEventHandle(audioEvent));
            check_hresult(audioClient->GetService(__uuidof(IAudioCaptureClient), audioCapture.put_void()));
            return true;
        } catch (...) {
            audioCapture = nullptr;
            audioClient = nullptr;
            return false;
        }
    }

    static DWORD WINAPI audioThunk(LPVOID s) { ((Session*)s)->audioLoop(); return 0; }
    void audioLoop() {
        LONGLONG written = 0; // audio frames put in the file so far; the file's audio clock
        auto write = [&](const BYTE* data, LONGLONG frames) { // data == nullptr writes silence
            while (frames > 0) {
                UINT32 n = UINT32(std::min<LONGLONG>(frames, kAudioRate / 10));
                com_ptr<IMFMediaBuffer> buf;
                com_ptr<IMFSample> sample;
                if (FAILED(MFCreateMemoryBuffer(n * kAudioBlock, buf.put())) || FAILED(MFCreateSample(sample.put()))) return;
                BYTE* dst = nullptr;
                if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return;
                if (data) memcpy(dst, data, n * kAudioBlock); else memset(dst, 0, n * kAudioBlock);
                buf->Unlock();
                buf->SetCurrentLength(n * kAudioBlock);
                sample->AddBuffer(buf.get());
                sample->SetSampleTime(written * 10000000 / kAudioRate);
                sample->SetSampleDuration(LONGLONG(n) * 10000000 / kAudioRate);
                {
                    std::lock_guard<std::mutex> lk(writerMx);
                    writer->WriteSample(aStream, sample.get());
                }
                written += n;
                frames -= n;
                if (data) data += n * kAudioBlock;
            }
        };
        audioClient->Start();
        for (;;) {
            bool stopping = audioStop.load();
            WaitForSingleObject(audioEvent, 20);
            const bool live = clockStarted.load();
            const LONGLONG start100 = live ? ticksTo100ns(t0) : 0;
            UINT32 packet = 0;
            while (SUCCEEDED(audioCapture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 qpcPos = 0;
                if (FAILED(audioCapture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos))) break;
                if (live) {
                    // Where this packet belongs on the file's clock: by its own timestamp
                    // when that is sane, otherwise by when it arrived.
                    LONGLONG nowFrames = (ticksTo100ns(qpc()) - start100) * kAudioRate / 10000000;
                    LONGLONG target = std::max(written, nowFrames - LONGLONG(frames) - kAudioRate / 50);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPos != 0) {
                        LONGLONG byStamp = (LONGLONG(qpcPos) - start100) * kAudioRate / 10000000;
                        if (llabs(byStamp - nowFrames) < kAudioRate) target = byStamp;
                    }
                    LONGLONG skip = 0; // part of the packet from before the recording began
                    if (target < 0) { skip = std::min<LONGLONG>(frames, -target); target += skip; }
                    if (LONGLONG(frames) > skip) {
                        if (target > written + kAudioRate / 100) write(nullptr, target - written); // the app was silent: fill the gap
                        write((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data + skip * kAudioBlock, LONGLONG(frames) - skip);
                    }
                }
                audioCapture->ReleaseBuffer(frames);
            }
            if (live) { // nothing is playing: keep the audio track moving so sound stays in sync
                LONGLONG nowFrames = (ticksTo100ns(qpc()) - start100) * kAudioRate / 10000000;
                if (nowFrames - written > kAudioRate / 5) write(nullptr, nowFrames - written - kAudioRate / 10);
            }
            if (stopping) break;
        }
        audioClient->Stop();
        LONGLONG end = end100ns.load();
        if (end > 0) {
            LONGLONG endFrames = end * kAudioRate / 10000000;
            if (endFrames > written) write(nullptr, endFrames - written);
        }
    }

    // ---------------------------------------------------------- file
    std::wstring makePath() {
        PWSTR videos = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos)) && videos) dir = videos;
        CoTaskMemFree(videos);
        if (dir.empty()) throw Fail{L"the Videos folder could not be found"};
        dir += L"\\SnapCap";
        if (!opt.folder.empty()) dir = opt.folder; // the user's own folder from the settings file
        while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) throw Fail{L"the output folder could not be created"};
        std::wstring name = p.name;
        for (auto& c : name)
            if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L' ';
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.' || name.back() == 0x2026)) name.pop_back();
        if (name.empty()) name = L"Recording";
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamp[64];
        swprintf(stamp, 64, L" %04d-%02d-%02d %02d-%02d-%02d.mp4", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return dir + L"\\" + name + stamp;
    }

    void createWriter(const std::wstring& path) {
        UINT token = 0;
        com_ptr<IMFDXGIDeviceManager> mgr;
        check_hresult(MFCreateDXGIDeviceManager(&token, mgr.put()));
        check_hresult(mgr->ResetDevice(d3d.get(), token));
        com_ptr<IMFAttributes> attrs;
        check_hresult(MFCreateAttributes(attrs.put(), 4));
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr.get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        check_hresult(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.get(), writer.put()));

        const UINT32 bitrate = opt.bitrateMbps > 0 ? UINT32(std::clamp(opt.bitrateMbps, 1, 150)) * 1000000u
                                                   : UINT32(std::clamp(double(encW) * encH * kFps * 0.1, 4e6, 60e6));
        com_ptr<IMFMediaType> vo, vi;
        check_hresult(MFCreateMediaType(vo.put()));
        vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vo->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vo.get(), MF_MT_FRAME_SIZE, encW, encH);
        MFSetAttributeRatio(vo.get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(vo.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        check_hresult(writer->AddStream(vo.get(), &vStream));
        check_hresult(MFCreateMediaType(vi.put()));
        vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        // Frames are handed over already converted (NV12, BT.709, 16-235), so the
        // writer adds no conversion stage of its own.
        vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        vi->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        vi->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        vi->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vi->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(vi.get(), MF_MT_FRAME_SIZE, encW, encH);
        MFSetAttributeRatio(vi.get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(vi.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        check_hresult(writer->SetInputMediaType(vStream, vi.get(), nullptr));

        if (hasAudio) {
            com_ptr<IMFMediaType> ao, ai;
            check_hresult(MFCreateMediaType(ao.put()));
            ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
            ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
            check_hresult(writer->AddStream(ao.get(), &aStream));
            check_hresult(MFCreateMediaType(ai.put()));
            ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
            ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kAudioBlock);
            ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * kAudioBlock);
            ai->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            check_hresult(writer->SetInputMediaType(aStream, ai.get(), nullptr));
        }
        check_hresult(writer->BeginWriting());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = encW; td.Height = encH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto targetProps = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        check_hresult(d3d->CreateTexture2D(&td, nullptr, canvas.put()));
        check_hresult(dc->CreateBitmapFromDxgiSurface(canvas.as<IDXGISurface>().get(), &targetProps, canvasBmp.put()));

        // The graphics card's own colour converter: BGRA canvas -> NV12 for the encoder.
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {UINT(kFps), 1};
        cd.InputWidth = encW; cd.InputHeight = encH;
        cd.OutputFrameRate = {UINT(kFps), 1};
        cd.OutputWidth = encW; cd.OutputHeight = encH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        check_hresult(vdev->CreateVideoProcessorEnumerator(&cd, vpEnum.put()));
        check_hresult(vdev->CreateVideoProcessor(vpEnum.get(), 0, vproc.put()));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        check_hresult(vdev->CreateVideoProcessorInputView(canvas.get(), vpEnum.get(), &ivd, canvasIn.put()));
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE csIn{}, csOut{};
        csIn.RGB_Range = 0;       // full range RGB in
        csOut.YCbCr_Matrix = 1;   // BT.709
        csOut.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vproc.get(), 0, &csIn);
        vctx->VideoProcessorSetOutputColorSpace(vproc.get(), &csOut);
        vctx->VideoProcessorSetStreamFrameFormat(vproc.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vproc.get(), 0, FALSE);

        td.Format = DXGI_FORMAT_NV12;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        for (auto& s : ring) {
            check_hresult(d3d->CreateTexture2D(&td, nullptr, s.tex.put()));
            check_hresult(vdev->CreateVideoProcessorOutputView(s.tex.get(), vpEnum.get(), &ovd, s.out.put()));
            check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), s.tex.get(), 0, FALSE, s.buf.put()));
            DWORD len = 0;
            if (auto b2 = s.buf.try_as<IMF2DBuffer>()) b2->GetContiguousLength(&len);
            s.buf->SetCurrentLength(len);
        }

        fontPx = std::max(14.0f, float(encH) / 54.0f);
        check_hresult(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, fontPx, L"", textFormat.put()));
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(16 / 255.f, 16 / 255.f, 16 / 255.f, 0.86f), brBox.put()));
        check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f), brName.put()));
        check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(120 / 255.f, 1.0f, 120 / 255.f), brFps.put()));
    }

    // ---------------------------------------------------------- one video frame
    // The part of the captured picture to record: the window's client area (no
    // title bar or border). Rechecked a few times a second.
    void updateCrop(UINT lw, UINT lh) {
        if (++cropAge < 15) return;
        cropAge = 0;
        crop = D2D1::RectF(0, 0, float(lw), float(lh));
        if (screen) return;
        RECT fb{}, cr{};
        POINT c{0, 0};
        if (IsIconic(p.target) || FAILED(DwmGetWindowAttribute(p.target, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb)))) return;
        if (!ClientToScreen(p.target, &c) || !GetClientRect(p.target, &cr)) return;
        float l = float(std::clamp<LONG>(c.x - fb.left, 0, lw)), t = float(std::clamp<LONG>(c.y - fb.top, 0, lh));
        float rgt = float(std::clamp<LONG>(c.x - fb.left + cr.right, 0, lw)), btm = float(std::clamp<LONG>(c.y - fb.top + cr.bottom, 0, lh));
        if (rgt - l >= 16 && btm - t >= 16) crop = D2D1::RectF(l, t, rgt, btm);
    }

    void drawText() {
        std::wstring name, fps;
        {
            std::lock_guard<std::mutex> lk(g_textMx);
            name = g_textName;
            fps = g_textFps;
        }
        if (name.empty() && fps.empty()) return;
        com_ptr<IDWriteTextLayout> ln, lf;
        if (FAILED(dw->CreateTextLayout(name.c_str(), UINT32(name.size()), textFormat.get(), 8192, 512, ln.put()))) return;
        if (FAILED(dw->CreateTextLayout(fps.c_str(), UINT32(fps.size()), textFormat.get(), 8192, 512, lf.put()))) return;
        DWRITE_TEXT_METRICS mn{}, mf{};
        ln->GetMetrics(&mn);
        lf->GetMetrics(&mf);
        const float margin = fontPx * 0.8f, padX = fontPx * 0.6f, padY = fontPx * 0.27f;
        const float h = std::max(mn.height, mf.height);
        const float bw = 2 * padX + mn.widthIncludingTrailingWhitespace + mf.widthIncludingTrailingWhitespace, bh = 2 * padY + h;
        const float x = (opt.corner & 1) ? float(encW) - margin - bw : margin; // right-hand corners
        const float y = (opt.corner & 2) ? float(encH) - margin - bh : margin; // bottom corners
        dc->FillRectangle(D2D1::RectF(x, y, x + bw, y + bh), brBox.get());
        dc->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), ln.get(), brName.get());
        dc->DrawTextLayout(D2D1::Point2F(x + padX + mn.widthIncludingTrailingWhitespace, y + padY), lf.get(), brFps.get());
    }

    // Scales the newest picture into `s` (keeping its shape, black bars if the
    // window changed shape since the recording began) and draws the text.
    bool compose(Slot& s) {
        std::lock_guard<std::mutex> lk(frameMx);
        if (!latest) return false;
        ID2D1Bitmap1* latestBmp = nullptr;
        for (auto& e : bmpCache) if (e.first == latest.get()) latestBmp = e.second.get();
        if (!latestBmp) { // capture reuses a handful of textures, so this happens only a few times
            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
            com_ptr<ID2D1Bitmap1> made;
            if (FAILED(dc->CreateBitmapFromDxgiSurface(latest.as<IDXGISurface>().get(), &props, made.put()))) return false;
            if (bmpCache.size() >= 8) bmpCache.clear();
            bmpCache.emplace_back(latest.get(), made);
            latestBmp = made.get();
        }
        D3D11_TEXTURE2D_DESC ld;
        latest->GetDesc(&ld);
        if (ld.Width != lastW || ld.Height != lastH) { lastW = ld.Width; lastH = ld.Height; cropAge = 1000; }
        updateCrop(ld.Width, ld.Height);
        const float sw = crop.right - crop.left, sh = crop.bottom - crop.top;
        const float scale = std::min(float(encW) / sw, float(encH) / sh);
        const float dw_ = sw * scale, dh = sh * scale;
        D2D1_RECT_F dest = D2D1::RectF((encW - dw_) / 2, (encH - dh) / 2, (encW + dw_) / 2, (encH + dh) / 2);
        if (fabsf(scale - 1.0f) < 0.002f) dest = D2D1::RectF(0, 0, sw, sh); // same size: copy pixel for pixel
        dc->SetTarget(canvasBmp.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0));
        dc->DrawBitmap(latestBmp, &dest, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, &crop);
        if (g_showInVideo.load()) drawText();
        HRESULT hr = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (FAILED(hr)) return false;
        // Convert for the encoder and send everything to the GPU as one batch. When a
        // game keeps the GPU busy, each separate batch waits its turn behind the game.
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = canvasIn.get();
        d2dLock->Enter();
        hr = vctx->VideoProcessorBlt(vproc.get(), s.out.get(), 0, 1, &st);
        ctx->Flush();
        d2dLock->Leave();
        return SUCCEEDED(hr);
    }

    void noteEncoder() {
        auto ex = writer.try_as<IMFSinkWriterEx>();
        if (!ex) return;
        for (DWORD idx = 0;; idx++) {
            GUID cat{};
            com_ptr<IMFTransform> mft;
            if (FAILED(ex->GetTransformForStream(vStream, idx, &cat, mft.put()))) break;
            com_ptr<IMFAttributes> ma;
            UINT32 n = 0;
            if (cat == MFT_CATEGORY_VIDEO_ENCODER && SUCCEEDED(mft->GetAttributes(ma.put())) && ma &&
                SUCCEEDED(ma->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &n)))
                r.hwEncoder = true;
        }
    }

    // Counts are per half second. Window capture is "starved" when it gets under about
    // 30 a second while whole-screen capture sees well over what it gets.
    static bool starved(int w, int d) { return d >= 4 && w < 15 && 2 * d > 3 * w + 4; }
    void useScreen(bool on) {
        if (screen.load() == on) return;
        screen = on;
        if (on) {
            std::lock_guard<std::mutex> lk(frameMx);
            releaseHeld(); // the next picture comes from whole-screen capture
            r.screenMethod = true;
        }
        r.switches++;
    }

    // ---------------------------------------------------------- the recording
    void run() {
        stage = L"starting the graphics device";
        createDevices();

        stage = L"starting window capture";
        RECT cr{};
        GetClientRect(p.target, &cr);
        if (!IsWindow(p.target) || IsIconic(p.target) || cr.right < 64 || cr.bottom < 64)
            throw Fail{L"the window is minimised or too small"};
        bool windowOk = true;
        try { startWindowCapture(); } catch (...) { windowOk = false; }
        bool have = windowOk && waitForFrame(1000);
        if (!stopRequested() && coversMonitor(p.target)) {
            // A fullscreen window. Windows may show it in a way window capture cannot see
            // (fullscreen OpenGL and Vulkan: one frame, then a trickle or nothing), and that
            // can change from moment to moment. Both methods are counted for half a second
            // and the recording uses the one really receiving the game's frames. The choice
            // is made once, here; see HISTORY.md for what that leaves open.
            duplThread = CreateThread(nullptr, 0, duplThunk, this, 0, nullptr);
            for (int waited = 0; waited < 2500 && !duplOpen.load() && !stopRequested(); waited += 20) Sleep(20);
            WaitForSingleObject(g_stop, 150);
            wPrev = wgcCount.load();
            dPrev = duplCount.load();
            WaitForSingleObject(g_stop, 500);
            int w = wgcCount.load() - wPrev, d = duplCount.load() - dPrev;
            wPrev += w;
            dPrev += d;
            r.probeWindow = w;
            r.probeScreen = d;
            if (starved(w, d) || !have) {
                useScreen(true);
                { std::lock_guard<std::mutex> lk(frameMx); haveFrame = false; }
                have = waitForFrame(2000);
            } else {
                // Window capture is the live one: close whole-screen capture again.
                // Leaving it open stalls the recording (measured: under 1 frame a second).
                duplStop = true;
                WaitForSingleObject(duplThread, 3000);
                CloseHandle(duplThread);
                duplThread = nullptr;
            }
        }
        if (stopRequested()) throw Fail{L"stopped before recording began"};
        if (!have) throw Fail{L"Windows gave no picture for this window"};

        if (screen) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(p.target, MONITOR_DEFAULTTONEAREST), &mi);
            cr = {0, 0, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top};
        }
        encW = cr.right & ~1;
        encH = cr.bottom & ~1;
        if (opt.maxHeight >= 240 && encH > opt.maxHeight) { // smaller video: scaled down, same shape
            encW = int(double(encW) * opt.maxHeight / encH + 0.5) & ~1;
            encH = opt.maxHeight & ~1;
        }

        r.width = encW;
        r.height = encH;

        stage = L"opening the application's audio";
        hasAudio = initAudio();
        r.audio = hasAudio;

        stage = L"creating the MP4 file";
        r.file = makePath();
        createWriter(r.file);

        stage = L"recording";
        t0 = qpc();
        { FILETIME ft; GetSystemTimePreciseAsFileTime(&ft); r.startUnix = double((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e7 - 11644473600.0; }
        clockStarted = true;
        g_state = Recording;
        if (hasAudio) audioThread = CreateThread(nullptr, 0, audioThunk, this, 0, nullptr);
        const LONGLONG dur = 10000000LL / kFps;
        LONGLONG last = -1;
        HRESULT writeErr = S_OK;
        while (!stopRequested() && IsWindow(p.target)) {
            LONGLONG i = (qpc() - t0) * kFps / qpf();
            if (i <= last) {
                LONGLONG due = t0 + (last + 1) * qpf() / kFps;
                LONGLONG ms = (due - qpc()) * 1000 / qpf();
                WaitForSingleObject(g_stop, DWORD(std::max<LONGLONG>(1, ms)));
                continue;
            }
            Slot* slot = nullptr;
            for (int k = 0; k < kRing && !slot; k++) {
                Slot& s = ring[(r.frames + r.dropped + k) % kRing];
                if (s.free()) slot = &s;
            }
            if (last >= 0 && i > last + 1) r.late += i - last - 1;
            last = i;
            if (!slot || !compose(*slot)) { r.dropped++; continue; } // encoder is behind: skip this frame
            com_ptr<IMFSample> sample;
            check_hresult(MFCreateSample(sample.put()));
            sample->AddBuffer(slot->buf.get());
            sample->SetSampleTime(i * dur);
            sample->SetSampleDuration(dur);
            {
                std::lock_guard<std::mutex> lk(writerMx);
                writeErr = writer->WriteSample(vStream, sample.get());
            }
            if (FAILED(writeErr)) break;
            r.frames++;
        }
        g_state = Stopping;
        end100ns = (last + 1) * dur;
        r.seconds = double(last + 1) / kFps;
        stopAudio();
        noteEncoder();
        stage = L"finishing the MP4 file";
        check_hresult(writeErr);
        check_hresult(writer->Finalize());
        writer = nullptr;
        if (r.frames == 0) throw Fail{L"no frames were recorded"};
        r.ok = true;
    }

    void stopAudio() {
        if (!audioThread) return;
        audioStop = true;
        SetEvent(audioEvent);
        WaitForSingleObject(audioThread, 5000);
        CloseHandle(audioThread);
        audioThread = nullptr;
    }

    void cleanup() {
        stopAudio();
        duplStop = true;
        if (duplThread) { WaitForSingleObject(duplThread, 3000); CloseHandle(duplThread); duplThread = nullptr; }
        writer = nullptr;
        if (!r.ok && !r.file.empty()) DeleteFileW(r.file.c_str()); // do not leave a broken file behind
        if (audioEvent) { CloseHandle(audioEvent); audioEvent = nullptr; }
    }

    void closeCapture() { // may be slow; done after the result has been reported
        { std::lock_guard<std::mutex> lk(frameMx); releaseHeld(); }
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }
};

DWORD WINAPI threadMain(LPVOID) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    timeBeginPeriod(1);
    auto s = std::make_unique<Session>();
    s->p = g_params;
    { std::lock_guard<std::mutex> lk(g_optsMx); s->opt = g_opts; }
    kFps = s->opt.fps == 30 ? 30 : 60;
    try {
        if (!mf) throw Fail{L"Media Foundation is not available"};
        s->run();
    } catch (Fail const& f) {
        s->r.error = f.msg;
    } catch (winrt::hresult_error const& e) {
        wchar_t code[32];
        swprintf(code, 32, L" (0x%08lx)", (unsigned long)e.code().value);
        s->r.error = std::wstring(L"failed while ") + s->stage + code;
    } catch (...) {
        s->r.error = std::wstring(L"failed while ") + s->stage;
    }
    s->cleanup();
    timeEndPeriod(1);
    {
        std::lock_guard<std::mutex> lk(g_resultMx);
        g_result = s->r;
    }
    AcquireSRWLockExclusive(&g_arrLock);
    g_arrivals.clear();
    ReleaseSRWLockExclusive(&g_arrLock);
    g_state = Idle;
    PostMessageW(s->p.notify, s->p.msg, 0, 0);
    s->closeCapture();
    s.reset();
    if (mf) MFShutdown();
    return 0;
}

} // namespace

bool start(HWND target, DWORD pid, const std::wstring& name, HWND notify, UINT doneMsg) {
    int idle = Idle;
    if (!g_state.compare_exchange_strong(idle, Starting)) return false;
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    ResetEvent(g_stop);
    g_params = Params{target, pid, name, notify, doneMsg};
    g_thread = CreateThread(nullptr, 0, threadMain, nullptr, 0, nullptr);
    if (!g_thread) { g_state = Idle; return false; }
    return true;
}

void stop() { SetEvent(g_stop); }

void shutdown() {
    SetEvent(g_stop);
    for (int i = 0; i < 300 && g_state.load() != Idle; i++) Sleep(50); // up to 15 s to finish the file
}

State state() { return State(g_state.load()); }

Result result() {
    std::lock_guard<std::mutex> lk(g_resultMx);
    return g_result;
}

void setOverlayText(const std::wstring& name, const std::wstring& fps) {
    std::lock_guard<std::mutex> lk(g_textMx);
    g_textName = name;
    g_textFps = fps;
}

void setShowInVideo(bool show) { g_showInVideo = show; }

void setOptions(const Options& o) {
    std::lock_guard<std::mutex> lk(g_optsMx);
    g_opts = o;
}

double arrivalFps() {
    LONGLONG now = qpc();
    AcquireSRWLockExclusive(&g_arrLock);
    while (!g_arrivals.empty() && g_arrivals.front() < now - qpf()) g_arrivals.pop_front();
    double v = double(g_arrivals.size());
    if (g_arrivals.size() >= 2 && g_arrivals.back() > g_arrivals.front())
        v = double(g_arrivals.size() - 1) * double(qpf()) / double(g_arrivals.back() - g_arrivals.front());
    ReleaseSRWLockExclusive(&g_arrLock);
    return v;
}

}
