// Test helper: opens an MP4 the way a player would and reports what is in it.
//   mp4check.exe file.mp4 [--at seconds] [--px x y]... [--freq hz]...
// Video: size, frame count, length, and the colour of chosen pixels in the
// frame at --at. Audio: length, when sound first starts, and how strong each
// --freq tone is (0 = absent; the test tones are about 0.08).
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring file = argv[1];
    double at = 2.0;
    std::vector<std::pair<int, int>> px;
    std::vector<double> freqs;
    std::wstring bmpPath;
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--at" && i + 1 < argc) at = _wtof(argv[++i]);
        else if (a == L"--px" && i + 2 < argc) { int x = _wtoi(argv[++i]); int y = _wtoi(argv[++i]); px.push_back({x, y}); }
        else if (a == L"--freq" && i + 1 < argc) freqs.push_back(_wtof(argv[++i]));
        else if (a == L"--bmp" && i + 1 < argc) bmpPath = argv[++i]; // save the frame at --at as a picture
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) { printf("{\"ok\":0,\"error\":\"open 0x%08lx\"}\n", (unsigned long)hr); return 1; }

    PROPVARIANT pv;
    PropVariantInit(&pv);
    double duration = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) duration = double(pv.uhVal.QuadPart) / 1e7;

    // ---- video
    UINT32 w = 0, h = 0;
    GUID vsub{};
    long long vframes = 0;
    double vlast = 0;
    std::string pixels;
    {
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
            native->GetGUID(MF_MT_SUBTYPE, &vsub);
        }
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
        bool sampled = false;
        while (SUCCEEDED(hr)) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (!s) continue;
            vframes++;
            vlast = double(ts) / 1e7;
            if (!sampled && vlast >= at && (!px.empty() || !bmpPath.empty())) {
                sampled = true;
                ComPtr<IMFMediaBuffer> b;
                s->ConvertToContiguousBuffer(&b);
                // Row 0 and row step: from the 2D interface when there is one, otherwise
                // from the stride the decoder reports (negative means bottom-up).
                ComPtr<IMF2DBuffer> b2;
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                bool is2d = SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
                BYTE* raw = nullptr;
                if (!is2d && SUCCEEDED(b->Lock(&raw, nullptr, nullptr))) {
                    ComPtr<IMFMediaType> cur;
                    UINT32 stride = w * 4;
                    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
                    pitch = LONG(stride);
                    scan0 = pitch < 0 ? raw + size_t(-pitch) * (h - 1) : raw;
                }
                if (scan0) {
                    for (auto& p : px) {
                        char one[64] = "\"out of range\"";
                        if (p.first >= 0 && p.second >= 0 && p.first < int(w) && p.second < int(h)) {
                            const BYTE* q = scan0 + LONG(p.second) * pitch + p.first * 4;
                            snprintf(one, sizeof(one), "\"%d,%d,%d\"", q[2], q[1], q[0]);
                        }
                        if (!pixels.empty()) pixels += ",";
                        pixels += one;
                    }
                    if (!bmpPath.empty()) {
                        BITMAPFILEHEADER fh{};
                        BITMAPINFOHEADER ih{};
                        ih.biSize = sizeof(ih); ih.biWidth = LONG(w); ih.biHeight = -LONG(h); ih.biPlanes = 1; ih.biBitCount = 32;
                        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + w * h * 4;
                        if (FILE* fp = _wfopen(bmpPath.c_str(), L"wb")) {
                            fwrite(&fh, sizeof(fh), 1, fp);
                            fwrite(&ih, sizeof(ih), 1, fp);
                            for (UINT32 y = 0; y < h; y++) fwrite(scan0 + LONG(y) * pitch, 4, w, fp);
                            fclose(fp);
                        }
                    }
                    if (is2d) b2->Unlock2D(); else b->Unlock();
                }
                if (false) {
                    b2->Unlock2D();
                }
            }
        }
    }

    // ---- audio
    bool hasAudio = false;
    double aSeconds = 0, firstSound = -1, lastSound = -1, rms = 0;
    std::string tones;
    {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t.Get());
        UINT32 rate = 48000, ch = 2;
        if (SUCCEEDED(hr)) {
            ComPtr<IMFMediaType> cur;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
                cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            }
            hasAudio = true;
            std::vector<float> mono;
            while (true) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> s;
                hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s);
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
                if (!s) continue;
                ComPtr<IMFMediaBuffer> b;
                s->ConvertToContiguousBuffer(&b);
                BYTE* d = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(b->Lock(&d, nullptr, &len))) {
                    const int16_t* v = (const int16_t*)d;
                    size_t frames = len / (2 * ch);
                    for (size_t i = 0; i < frames; i++) {
                        float m = 0;
                        for (UINT32 c = 0; c < ch; c++) m += v[i * ch + c] / 32768.0f;
                        mono.push_back(m / ch);
                    }
                    b->Unlock();
                }
            }
            aSeconds = double(mono.size()) / rate;
            double sum = 0;
            for (size_t i = 0; i < mono.size(); i++) {
                if (firstSound < 0 && fabsf(mono[i]) > 0.01f) firstSound = double(i) / rate;
                if (fabsf(mono[i]) > 0.01f) lastSound = double(i) / rate;
                sum += double(mono[i]) * mono[i];
            }
            if (!mono.empty()) rms = sqrt(sum / mono.size());
            // strength of each tone over the part after sound begins
            size_t from = firstSound >= 0 ? size_t((firstSound + 0.2) * rate) : 0;
            for (double f : freqs) {
                double re = 0, im = 0;
                size_t n = 0;
                for (size_t i = from; i < mono.size(); i++, n++) {
                    double ph = 2.0 * 3.14159265358979 * f * double(i) / rate;
                    re += mono[i] * cos(ph);
                    im += mono[i] * sin(ph);
                }
                double amp = n ? 2.0 * sqrt(re * re + im * im) / n : 0;
                char one[64];
                snprintf(one, sizeof(one), "%s\"%g\":%.4f", tones.empty() ? "" : ",", f, amp);
                tones += one;
            }
        }
    }
    printf("{\"ok\":1,\"duration\":%.3f,\"w\":%u,\"h\":%u,\"h264\":%d,\"video_frames\":%lld,\"video_last\":%.3f,\"pixels\":[%s],"
           "\"audio\":%d,\"audio_seconds\":%.3f,\"first_sound\":%.3f,\"last_sound\":%.3f,\"rms\":%.4f,\"tones\":{%s}}\n",
           duration, w, h, vsub == MFVideoFormat_H264 ? 1 : 0, vframes, vlast, pixels.c_str(),
           hasAudio ? 1 : 0, aSeconds, firstSound, lastSound, rms, tones.c_str());
    MFShutdown();
    return 0;
}
