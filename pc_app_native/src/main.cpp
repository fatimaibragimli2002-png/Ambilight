/*
 * Ambilight native sender (C++) — DXGI Desktop Duplication capture.
 * 73 LEDs: 19 left + 35 top + 19 right, Adalight serial protocol.
 *
 * Build: cmake -S . -B build -G Ninja && cmake --build build --config Release
 *
 * or
 *
 * cmd.exe /c "call ""E:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cd /d C:\Users\Arsalan\Documents\PlatformIO\Projects\Ambilight\pc_app_native && ""C:\Program Files\CMake\bin\cmake.exe"" --build build"
 *
 * or
 *
 * cmake --build build
 *
 * Run:   ambilight_native.exe --port COM3 --fps 30 --monitor 0 --profile
 * or
 * .\build\ambilight_native.exe --profile --smoothing 0.35 --fps 60 --baud 200000 --edge-depth 60
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <timeapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

constexpr int LEDS_LEFT = 19;
constexpr int LEDS_TOP = 35;
constexpr int LEDS_RIGHT = 19;
constexpr int LEDS_TOTAL = LEDS_LEFT + LEDS_TOP + LEDS_RIGHT;
constexpr int DATA_SIZE = LEDS_TOTAL * 3;

struct AppConfig {
  // Runtime behavior
  static constexpr double kStatusPrintIntervalSec = 3.0;
  static constexpr int kSerialBootWaitMs = 1000;
  static constexpr int kKeepAliveMs = 1000;
  static constexpr int kLowPowerAfterMs = 1500;
  static constexpr int kLowPowerCaptureFps = 1;
  static constexpr int kLowPowerDownscale = 8;

  // Defaults
  static constexpr const char* kDefaultPort = "COM3";
  static constexpr int kDefaultMonitor = 0;
  static constexpr int kDefaultFps = 60;
  static constexpr int kDefaultBaud = 200000;
  static constexpr int kDefaultEdgeDepth = 60;
  static constexpr double kDefaultSmoothing = 0.35;
  static constexpr int kDefaultDownscale = 4;

  // Limits / normalization
  static constexpr int kMinMonitor = 0;
  static constexpr int kMinFps = 1;
  static constexpr int kMinBaud = 115200;
  static constexpr int kMinEdgeDepth = 10;
  static constexpr int kMinDownscale = 1;
  static constexpr double kMinSmoothing = 0.0;
  static constexpr double kMaxSmoothing = 1.0;
};

std::atomic_bool g_running{true};

template <typename T>
void SafeRelease(T*& ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

struct Region {
  int y0;
  int y1;
  int x0;
  int x1;
};

struct Options {
  std::string port;
  int monitor;
  int fps;
  int baud;
  int edgeDepth;
  double smoothing;
  int downscale;
  bool profile = false;
  bool listPorts = false;
  bool visualize = false;
};

Options MakeDefaultOptions() {
  Options options{};
  options.port = AppConfig::kDefaultPort;
  options.monitor = AppConfig::kDefaultMonitor;
  options.fps = AppConfig::kDefaultFps;
  options.baud = AppConfig::kDefaultBaud;
  options.edgeDepth = AppConfig::kDefaultEdgeDepth;
  options.smoothing = AppConfig::kDefaultSmoothing;
  options.downscale = AppConfig::kDefaultDownscale;
  return options;
}

void NormalizeOptions(Options& options) {
  options.fps = std::max(AppConfig::kMinFps, options.fps);
  options.baud = std::max(AppConfig::kMinBaud, options.baud);
  options.edgeDepth = std::max(AppConfig::kMinEdgeDepth, options.edgeDepth);
  options.downscale = std::max(AppConfig::kMinDownscale, options.downscale);
  options.smoothing = std::clamp(options.smoothing, AppConfig::kMinSmoothing, AppConfig::kMaxSmoothing);
  options.monitor = std::max(AppConfig::kMinMonitor, options.monitor);
}

enum class ParseStatus {
  Ok,
  ExitSuccess,
  ExitError,
};

BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_running.store(false);
    return TRUE;
  }
  return FALSE;
}

void PrintUsage(const char* exeName) {
  std::cout
      << "Ambilight native sender (C++)\n\n"
      << "Usage:\n"
      << "  " << exeName << " [options]\n\n"
      << "Options:\n"
      << "  --port " << AppConfig::kDefaultPort << "              Serial port\n"
      << "  --monitor " << AppConfig::kDefaultMonitor << "              Monitor index (0-based)\n"
      << "  --fps " << AppConfig::kDefaultFps << "                 Target frames per second\n"
      << "  --baud " << AppConfig::kDefaultBaud << "            Serial baud rate\n"
      << "  --edge-depth " << AppConfig::kDefaultEdgeDepth << "          Pixels sampled from edge\n"
      << "  --downscale " << AppConfig::kDefaultDownscale << "            Step size while sampling (1 = full)\n"
      << "  --smoothing " << AppConfig::kDefaultSmoothing << "          EMA smoothing [0..1], 1 disables\n"
      << "  --profile                Print average stage timings\n"
      << "  --visualize              Overlay sampling regions on screen\n"
      << "  --list-ports             List COM ports and exit\n"
      << "  --help                   Show this help\n";
}

bool ParseIntArg(const std::string& text, int& out) {
  try {
    size_t pos = 0;
    int value = std::stoi(text, &pos, 10);
    if (pos != text.size()) {
      return false;
    }
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDoubleArg(const std::string& text, double& out) {
  try {
    size_t pos = 0;
    double value = std::stod(text, &pos);
    if (pos != text.size()) {
      return false;
    }
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

ParseStatus ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    auto nextValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return ParseStatus::ExitSuccess;
    }
    if (arg == "--list-ports") {
      options.listPorts = true;
      continue;
    }
    if (arg == "--profile") {
      options.profile = true;
      continue;
    }
    if (arg == "--visualize") {
      options.visualize = true;
      continue;
    }
    if (arg == "--port" || arg == "-p") {
      const char* value = nextValue("--port");
      if (!value) {
        return ParseStatus::ExitError;
      }
      options.port = value;
      continue;
    }
    if (arg == "--monitor" || arg == "-m") {
      const char* value = nextValue("--monitor");
      if (!value || !ParseIntArg(value, options.monitor)) {
        std::cerr << "Invalid --monitor value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }
    if (arg == "--fps" || arg == "-f") {
      const char* value = nextValue("--fps");
      if (!value || !ParseIntArg(value, options.fps)) {
        std::cerr << "Invalid --fps value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }
    if (arg == "--baud") {
      const char* value = nextValue("--baud");
      if (!value || !ParseIntArg(value, options.baud)) {
        std::cerr << "Invalid --baud value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }
    if (arg == "--edge-depth") {
      const char* value = nextValue("--edge-depth");
      if (!value || !ParseIntArg(value, options.edgeDepth)) {
        std::cerr << "Invalid --edge-depth value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }
    if (arg == "--downscale") {
      const char* value = nextValue("--downscale");
      if (!value || !ParseIntArg(value, options.downscale)) {
        std::cerr << "Invalid --downscale value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }
    if (arg == "--smoothing") {
      const char* value = nextValue("--smoothing");
      if (!value || !ParseDoubleArg(value, options.smoothing)) {
        std::cerr << "Invalid --smoothing value\n";
        return ParseStatus::ExitError;
      }
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    PrintUsage(argv[0]);
    return ParseStatus::ExitError;
  }

  NormalizeOptions(options);

  return ParseStatus::Ok;
}

void ListComPorts() {
  std::cout << "Detected COM ports:\n";
  bool found = false;

  for (int i = 1; i <= 256; ++i) {
    const std::string name = "COM" + std::to_string(i);
    char targetPath[256] = {};
    if (QueryDosDeviceA(name.c_str(), targetPath, static_cast<DWORD>(sizeof(targetPath))) != 0) {
      std::cout << "  " << name << "\n";
      found = true;
    }
  }

  if (!found) {
    std::cout << "  (none found)\n";
  }
}

std::string FormatDuration(double ms) {
  std::ostringstream os;
  os << std::fixed;
  if (ms >= 1000.0) {
    os << std::setprecision(3) << (ms / 1000.0) << "s";
  } else {
    os << std::setprecision(3) << ms << "ms";
  }
  return os.str();
}

std::string FormatNumber(double value, int precision = 2) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(precision) << value;
  return os.str();
}

void PrintStatusLine(const std::string& line) {
  static size_t lastLen = 0;
  std::cout << "\r" << line;
  if (line.size() < lastLen) {
    std::cout << std::string(lastLen - line.size(), ' ');
  }
  std::cout << std::flush;
  lastLen = line.size();
}

// ── Embedded HLSL compute shader ──────────────────────────
// Runs entirely on GPU: reads desktop texture, averages each LED region,
// writes float4 per LED.  Only ~1 KB read back instead of 8 MB full frame.
static const char kRegionCS[] = R"HLSL(
struct Region { int y0; int y1; int x0; int x1; };

Texture2D<float4>          Tex  : register(t0);
StructuredBuffer<Region>   Regs : register(t1);
RWStructuredBuffer<float4> Out  : register(u0);

cbuffer CB : register(b0) { uint Step; uint3 _p; };

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Region r = Regs[id.x];
    float3 s = float3(0, 0, 0);
    uint   n = 0;
    for (int y = r.y0; y < r.y1; y += (int)Step) {
        for (int x = r.x0; x < r.x1; x += (int)Step) {
            s += Tex.Load(int3(x, y, 0)).rgb;
            ++n;
        }
    }
    n = max(n, 1u);
    Out[id.x] = float4(s / (float)n, 0);
}
)HLSL";

// ── DXGI Desktop Duplication + GPU compute ─────────────────
class DxgiCapture {
 public:
  ~DxgiCapture() { Cleanup(); }

  bool Init(int outputIndex) {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device_, &fl, &ctx_);
    if (FAILED(hr)) {
      std::cerr << "D3D11CreateDevice failed\n";
      return false;
    }

    IDXGIDevice* dd = nullptr;
    device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd));
    IDXGIAdapter* adapter = nullptr;
    dd->GetAdapter(&adapter);
    SafeRelease(dd);

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(outputIndex, &output);
    SafeRelease(adapter);
    if (FAILED(hr)) {
      std::cerr << "Monitor " << outputIndex << " not found\n";
      return false;
    }

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);
    width_ = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    originX_ = desc.DesktopCoordinates.left;
    originY_ = desc.DesktopCoordinates.top;

    IDXGIOutput1* out1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1));
    SafeRelease(output);
    if (FAILED(hr))
      return false;

    hr = out1->DuplicateOutput(device_, &dup_);
    SafeRelease(out1);
    if (FAILED(hr)) {
      std::cerr << "DuplicateOutput failed\n";
      return false;
    }

    // GPU-only texture (SRV-bindable) for desktop frame copy
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width_);
    td.Height = static_cast<UINT>(height_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device_->CreateTexture2D(&td, nullptr, &gpuTex_);
    if (FAILED(hr))
      return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(gpuTex_, &sd, &texSrv_);
    if (FAILED(hr))
      return false;

    return true;
  }

  bool InitCompute(const Region* regions, int numRegions, int downscale) {
    ReleaseComputeResources();
    numLeds_ = numRegions;
    HRESULT hr;

    // Compile compute shader at startup
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3DCompile(kRegionCS, sizeof(kRegionCS) - 1, "RegionCS",
                    nullptr, nullptr, "CSMain", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
      if (err) {
        std::cerr << "Shader: " << static_cast<char*>(err->GetBufferPointer()) << "\n";
        err->Release();
      }
      return false;
    }
    if (err)
      err->Release();

    hr = device_->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs_);
    blob->Release();
    if (FAILED(hr))
      return false;

    // Regions structured buffer + SRV
    {
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = static_cast<UINT>(numLeds_ * sizeof(Region));
      bd.Usage = D3D11_USAGE_IMMUTABLE;
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = sizeof(Region);
      D3D11_SUBRESOURCE_DATA init{};
      init.pSysMem = regions;
      hr = device_->CreateBuffer(&bd, &init, &regBuf_);
      if (FAILED(hr))
        return false;

      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_UNKNOWN;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.NumElements = static_cast<UINT>(numLeds_);
      hr = device_->CreateShaderResourceView(regBuf_, &sd, &regSrv_);
      if (FAILED(hr))
        return false;
    }

    // Output structured buffer (float4 per LED) + UAV
    {
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = static_cast<UINT>(numLeds_ * 16);
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      bd.StructureByteStride = 16;
      hr = device_->CreateBuffer(&bd, nullptr, &outBuf_);
      if (FAILED(hr))
        return false;

      D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
      ud.Format = DXGI_FORMAT_UNKNOWN;
      ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      ud.Buffer.NumElements = static_cast<UINT>(numLeds_);
      hr = device_->CreateUnorderedAccessView(outBuf_, &ud, &outUav_);
      if (FAILED(hr))
        return false;
    }

    // Staging buffer for GPU -> CPU readback (~1 KB vs 8 MB before)
    {
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = static_cast<UINT>(numLeds_ * 16);
      bd.Usage = D3D11_USAGE_STAGING;
      bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = device_->CreateBuffer(&bd, nullptr, &stageBuf_);
      if (FAILED(hr))
        return false;
    }

    // Constants (downscale step)
    {
      struct alignas(16) CB {
        uint32_t step;
        uint32_t pad[3];
      };
      CB cb = {static_cast<uint32_t>(downscale), {}};
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = 16;
      bd.Usage = D3D11_USAGE_IMMUTABLE;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init{};
      init.pSysMem = &cb;
      hr = device_->CreateBuffer(&bd, &init, &cbBuf_);
      if (FAILED(hr))
        return false;
    }

    return true;
  }

  // Returns: 1 = new frame processed, 0 = no new frame, -1 = error
  int GrabAndProcess(uint8_t* outRgb) {
    if (!dup_)
      return -1;

    IDXGIResource* res = nullptr;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    HRESULT hr = dup_->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
      return 0;
    if (FAILED(hr))
      return -1;

    // Copy desktop frame to GPU-only texture and release immediately
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    SafeRelease(res);
    ctx_->CopyResource(gpuTex_, tex);
    SafeRelease(tex);
    dup_->ReleaseFrame();

    // Dispatch compute shader — averages regions entirely on GPU
    ctx_->CSSetShader(cs_, nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {texSrv_, regSrv_};
    ctx_->CSSetShaderResources(0, 2, srvs);
    ctx_->CSSetUnorderedAccessViews(0, 1, &outUav_, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, &cbBuf_);
    ctx_->Dispatch(static_cast<UINT>(numLeds_), 1, 1);

    // Unbind
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    ctx_->CSSetShaderResources(0, 2, nullSrvs);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

    // Readback only ~1 KB (73 x float4) instead of 8 MB full frame
    ctx_->CopyResource(stageBuf_, outBuf_);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = ctx_->Map(stageBuf_, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
      return -1;

    const float* f = static_cast<const float*>(map.pData);
    for (int i = 0; i < numLeds_; ++i) {
      outRgb[i * 3 + 0] = static_cast<uint8_t>(std::min(f[i * 4 + 0] * 255.0f + 0.5f, 255.0f));
      outRgb[i * 3 + 1] = static_cast<uint8_t>(std::min(f[i * 4 + 1] * 255.0f + 0.5f, 255.0f));
      outRgb[i * 3 + 2] = static_cast<uint8_t>(std::min(f[i * 4 + 2] * 255.0f + 0.5f, 255.0f));
    }
    ctx_->Unmap(stageBuf_, 0);

    return 1;
  }

  int Width() const { return width_; }
  int Height() const { return height_; }
  int OriginX() const { return originX_; }
  int OriginY() const { return originY_; }

  void Cleanup() {
    ReleaseComputeResources();
    SafeRelease(texSrv_);
    SafeRelease(gpuTex_);
    SafeRelease(dup_);
    SafeRelease(ctx_);
    SafeRelease(device_);
  }

 private:
  void ReleaseComputeResources() {
    SafeRelease(cbBuf_);
    SafeRelease(stageBuf_);
    SafeRelease(outUav_);
    SafeRelease(outBuf_);
    SafeRelease(regSrv_);
    SafeRelease(regBuf_);
    SafeRelease(cs_);
  }

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  IDXGIOutputDuplication* dup_ = nullptr;
  ID3D11Texture2D* gpuTex_ = nullptr;
  ID3D11ShaderResourceView* texSrv_ = nullptr;
  ID3D11ComputeShader* cs_ = nullptr;
  ID3D11Buffer* regBuf_ = nullptr;
  ID3D11ShaderResourceView* regSrv_ = nullptr;
  ID3D11Buffer* outBuf_ = nullptr;
  ID3D11UnorderedAccessView* outUav_ = nullptr;
  ID3D11Buffer* stageBuf_ = nullptr;
  ID3D11Buffer* cbBuf_ = nullptr;
  int width_ = 0, height_ = 0, numLeds_ = 0;
  int originX_ = 0, originY_ = 0;
};

// ── Sampling-region overlay (--visualize) ─────────────────
// Layered, click-through, always-on-top window.  Per frame draws:
//  • Semi-transparent region fill in the live LED color
//  • 3-layer outline: 1px black outer → 2px colored inner (contrast on any content)
//  • Circle at centroid: dark outer ring → white ring → LED color → white center
//  • "L00"-padded badge with a dark background, placed away from the screen edge
//    (right of dot for left strip, below for top strip, left for right strip)
// Uses UpdateLayeredWindow + pre-multiplied alpha — no GDI+ required.
class OverlayWindow {
 public:
  bool Create(int monX, int monY, int w, int h) {
    x_ = monX;
    y_ = monY;
    w_ = w;
    h_ = h;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AmbilightViz";
    RegisterClassExW(&wc);  // ignore failure on re-register

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"AmbilightViz", L"Ambilight Visualizer", WS_POPUP,
        monX, monY, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_)
      return false;

    // Prevent feedback loop: if overlay is captured, it appears as constant motion and
    // low-power mode never activates. Ignore failure on unsupported Windows versions.
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS,
                            reinterpret_cast<void**>(&bits_), nullptr, 0);
    oldBmp_ = static_cast<HBITMAP>(SelectObject(memDC_, dib_));
    font_ = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return bits_ != nullptr;
  }

  void Draw(const Region* regions, int n, const uint8_t* rgb) {
    memset(bits_, 0, static_cast<size_t>(w_) * h_ * 4);

    for (int i = 0; i < n; ++i) {
      uint8_t r = rgb[i * 3 + 0];
      uint8_t g = rgb[i * 3 + 1];
      uint8_t b = rgb[i * 3 + 2];
      BoostDark(r, g, b);
      DrawRegionFill(regions[i], r, g, b);
      DrawRegionOutline(regions[i], r, g, b);
      DrawDot(regions[i], r, g, b);
      DrawLabel(regions[i], i, r, g, b);
    }

    POINT ptSrc{0, 0}, ptDst{x_, y_};
    SIZE sz{w_, h_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &ptDst, &sz,
                        memDC_, &ptSrc, 0, &blend, ULW_ALPHA);
  }

  void Destroy() {
    if (font_) {
      DeleteObject(font_);
      font_ = nullptr;
    }
    if (dib_) {
      SelectObject(memDC_, oldBmp_);
      DeleteObject(dib_);
      dib_ = nullptr;
    }
    if (memDC_) {
      DeleteDC(memDC_);
      memDC_ = nullptr;
    }
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

 private:
  // Lift very dark colors so outlines remain visible on any background.
  static void BoostDark(uint8_t& r, uint8_t& g, uint8_t& b) {
    const int m = std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
    if (m == 0) {
      r = g = b = 90;
      return;
    }
    if (m < 70) {
      const float s = 70.0f / static_cast<float>(m);
      r = static_cast<uint8_t>(std::min(255, static_cast<int>(r * s)));
      g = static_cast<uint8_t>(std::min(255, static_cast<int>(g * s)));
      b = static_cast<uint8_t>(std::min(255, static_cast<int>(b * s)));
    }
  }

  // Write one pre-multiplied BGRA pixel.
  void PutPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= w_ || y < 0 || y >= h_)
      return;
    uint8_t* p = bits_ + (static_cast<ptrdiff_t>(y) * w_ + x) * 4;
    p[0] = static_cast<uint8_t>(b * a / 255u);
    p[1] = static_cast<uint8_t>(g * a / 255u);
    p[2] = static_cast<uint8_t>(r * a / 255u);
    p[3] = a;
  }

  void DrawHLine(int x0, int x1, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int x = x0; x < x1; ++x)
      PutPixel(x, y, r, g, b, a);
  }
  void DrawVLine(int y0, int y1, int x, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = y0; y < y1; ++y)
      PutPixel(x, y, r, g, b, a);
  }

  void FillRect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    x0 = std::clamp(x0, 0, w_);
    x1 = std::clamp(x1, 0, w_);
    y0 = std::clamp(y0, 0, h_);
    y1 = std::clamp(y1, 0, h_);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        PutPixel(x, y, r, g, b, a);
  }

  // Filled circle using squared-distance test.
  void DrawCircle(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx)
        if (dx * dx + dy * dy <= radius * radius)
          PutPixel(cx + dx, cy + dy, r, g, b, a);
  }

  void DrawRegionFill(const Region& reg, uint8_t r, uint8_t g, uint8_t b) {
    // Inset by 3px so the fill sits inside the outline.
    FillRect(reg.x0 + 3, reg.y0 + 3, reg.x1 - 3, reg.y1 - 3, r, g, b, 38);
  }

  void DrawRegionOutline(const Region& reg, uint8_t r, uint8_t g, uint8_t b) {
    const int y0 = std::clamp(reg.y0, 0, h_ - 1);
    const int y1 = std::clamp(reg.y1, 0, h_);
    const int x0 = std::clamp(reg.x0, 0, w_ - 1);
    const int x1 = std::clamp(reg.x1, 0, w_);
    // Outermost pixel: black, contrasts against any screen content.
    DrawHLine(x0, x1, y0, 0, 0, 0, 210);
    DrawHLine(x0, x1, y1 - 1, 0, 0, 0, 210);
    DrawVLine(y0, y1, x0, 0, 0, 0, 210);
    DrawVLine(y0, y1, x1 - 1, 0, 0, 0, 210);
    // Inner 2 pixels: LED color at high opacity.
    for (int t = 1; t <= 2; ++t) {
      DrawHLine(x0 + t, x1 - t, y0 + t, r, g, b, 235);
      DrawHLine(x0 + t, x1 - t, y1 - 1 - t, r, g, b, 235);
      DrawVLine(y0 + t, y1 - t, x0 + t, r, g, b, 235);
      DrawVLine(y0 + t, y1 - t, x1 - 1 - t, r, g, b, 235);
    }
  }

  void DrawDot(const Region& reg, uint8_t r, uint8_t g, uint8_t b) {
    const int cx = (reg.x0 + reg.x1) / 2;
    const int cy = (reg.y0 + reg.y1) / 2;
    DrawCircle(cx, cy, 8, 0, 0, 0, 210);        // dark outer ring (contrast)
    DrawCircle(cx, cy, 7, 255, 255, 255, 255);  // white ring
    DrawCircle(cx, cy, 6, r, g, b, 255);        // LED color fill
    DrawCircle(cx, cy, 2, 255, 255, 255, 255);  // bright center dot
  }

  void DrawLabel(const Region& reg, int idx, uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {
    const int cx = (reg.x0 + reg.x1) / 2;
    const int cy = (reg.y0 + reg.y1) / 2;

    // Place badge relative to which strip the LED belongs to.
    int tx, ty;
    if (idx < LEDS_LEFT) {
      // Left strip: badge to the right of the dot.
      tx = cx + 11;
      ty = cy - 8;
    } else if (idx < LEDS_LEFT + LEDS_TOP) {
      // Top strip: badge below the dot.
      tx = cx - 15;
      ty = cy + 11;
    } else {
      // Right strip: badge to the left of the dot.
      tx = cx - 38;
      ty = cy - 8;
    }
    tx = std::clamp(tx, 1, w_ - 39);
    ty = std::clamp(ty, 1, h_ - 18);

    // Dark semi-transparent badge background.
    FillRect(tx - 2, ty - 2, tx + 36, ty + 17, 0, 0, 0, 175);

    if (!font_)
      return;
    HFONT oldFont = static_cast<HFONT>(SelectObject(memDC_, font_));
    SetBkMode(memDC_, TRANSPARENT);

    // Zero-padded: "L00"…"L72"
    const std::string numStr = (idx < 10 ? "0" : "") + std::to_string(idx);
    const std::wstring wlbl = L"L" + std::wstring(numStr.begin(), numStr.end());

    // White text — always readable against the dark badge.
    SetTextColor(memDC_, RGB(255, 255, 255));
    RECT rc{tx, ty, tx + 38, ty + 17};
    DrawTextW(memDC_, wlbl.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE);

    SelectObject(memDC_, oldFont);

    // GDI writes RGB but leaves alpha=0 (TRANSPARENT bg mode).
    // Any pixel GDI touched has alpha=0 but non-zero color → set alpha=255.
    const int bx0 = std::max(0, tx);
    const int by0 = std::max(0, ty);
    const int bx1 = std::min(w_, tx + 38);
    const int by1 = std::min(h_, ty + 17);
    for (int py = by0; py < by1; ++py) {
      uint8_t* row = bits_ + static_cast<ptrdiff_t>(py) * w_ * 4;
      for (int px = bx0; px < bx1; ++px) {
        uint8_t* p = row + px * 4;
        if (p[3] == 0 && (p[0] | p[1] | p[2]))
          p[3] = 255;
      }
    }
  }

  HWND hwnd_ = nullptr;
  HDC memDC_ = nullptr;
  HBITMAP dib_ = nullptr;
  HBITMAP oldBmp_ = nullptr;
  HFONT font_ = nullptr;
  uint8_t* bits_ = nullptr;
  int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
};

class SerialPort {
 public:
  ~SerialPort() {
    Close();
  }

  bool Open(const std::string& portName, int baudRate) {
    std::string device = portName;
    if (device.rfind("\\\\.\\", 0) != 0) {
      device = "\\\\.\\" + device;
    }

    handle_ = CreateFileA(
        device.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
      std::cerr << "Failed to open " << device << " (error " << GetLastError() << ")\n";
      return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(handle_, &dcb)) {
      std::cerr << "GetCommState failed (error " << GetLastError() << ")\n";
      Close();
      return false;
    }

    dcb.BaudRate = static_cast<DWORD>(baudRate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;

    if (!SetCommState(handle_, &dcb)) {
      std::cerr << "SetCommState failed (error " << GetLastError() << ")\n";
      Close();
      return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.WriteTotalTimeoutConstant = 50;
    SetCommTimeouts(handle_, &timeouts);

    PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
  }

  bool Write(const uint8_t* data, size_t len) {
    if (handle_ == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD written = 0;
    const DWORD toWrite = static_cast<DWORD>(len);
    if (!WriteFile(handle_, data, toWrite, &written, nullptr)) {
      return false;
    }
    return written == toWrite;
  }

  void Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::array<uint8_t, 6> BuildAdalightHeader() {
  std::array<uint8_t, 6> header{};
  const int count = LEDS_TOTAL - 1;
  const uint8_t hi = static_cast<uint8_t>((count >> 8) & 0xFF);
  const uint8_t lo = static_cast<uint8_t>(count & 0xFF);

  header[0] = static_cast<uint8_t>('A');
  header[1] = static_cast<uint8_t>('d');
  header[2] = static_cast<uint8_t>('a');
  header[3] = hi;
  header[4] = lo;
  header[5] = static_cast<uint8_t>(hi ^ lo ^ 0x55);
  return header;
}

std::vector<Region> BuildSampleRegions(int width, int height, int edgeDepth) {
  const int segH = std::max(1, height / LEDS_LEFT);
  const int segW = std::max(1, width / LEDS_TOP);
  const int depth = std::max(1, std::min(edgeDepth, std::min(width, height)));

  std::vector<Region> regions;
  regions.reserve(LEDS_TOTAL);

  for (int i = 0; i < LEDS_LEFT; ++i) {
    const int y0 = (LEDS_LEFT - 1 - i) * segH;
    const int y1 = y0 + segH;
    regions.push_back({y0, y1, 0, depth});
  }

  for (int i = 0; i < LEDS_TOP; ++i) {
    const int x0 = i * segW;
    const int x1 = x0 + segW;
    regions.push_back({0, depth, x0, x1});
  }

  for (int i = 0; i < LEDS_RIGHT; ++i) {
    const int y0 = i * segH;
    const int y1 = y0 + segH;
    regions.push_back({y0, y1, width - depth, width});
  }

  for (auto& region : regions) {
    region.y0 = std::clamp(region.y0, 0, height - 1);
    region.y1 = std::clamp(region.y1, region.y0 + 1, height);
    region.x0 = std::clamp(region.x0, 0, width - 1);
    region.x1 = std::clamp(region.x1, region.x0 + 1, width);
  }

  return regions;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt = MakeDefaultOptions();
  auto st = ParseArgs(argc, argv, opt);
  if (st == ParseStatus::ExitSuccess)
    return 0;
  if (st == ParseStatus::ExitError)
    return 1;
  if (opt.listPorts) {
    ListComPorts();
    return 0;
  }

  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  timeBeginPeriod(1);

  // ── DXGI capture + GPU compute pipeline ──
  DxgiCapture capture;
  if (!capture.Init(opt.monitor)) {
    std::cerr << "DXGI capture init failed\n";
    timeEndPeriod(1);
    return 1;
  }

  const int W = capture.Width(), H = capture.Height();
  auto regions = BuildSampleRegions(W, H, opt.edgeDepth);

  if (!capture.InitCompute(regions.data(), static_cast<int>(regions.size()), opt.downscale)) {
    std::cerr << "GPU compute pipeline init failed\n";
    timeEndPeriod(1);
    return 1;
  }

  // ── Serial throughput check ──
  constexpr size_t PKT = 6 + DATA_SIZE;

  // ── Overlay (--visualize) ──
  OverlayWindow overlay;
  if (opt.visualize) {
    if (!overlay.Create(capture.OriginX(), capture.OriginY(), W, H))
      std::cerr << "Warning: overlay window creation failed (continuing without)\n";
    else
      std::cout << "Visualizer overlay active - press Ctrl+C to stop\n";
  }

  const int maxSerialFps = opt.baud / 10 / static_cast<int>(PKT);
  if (opt.fps > maxSerialFps) {
    std::cout << "Warning: serial baud " << opt.baud
              << " caps throughput at ~" << maxSerialFps
              << " FPS (requested " << opt.fps
              << "). Increase --baud to match firmware.\n";
  }

  std::cout
      << "Monitor " << opt.monitor << ": " << W << "x" << H
      << " | " << LEDS_TOTAL << " LEDs | " << opt.fps << " FPS"
      << " | downscale x" << opt.downscale << " | smoothing " << opt.smoothing
      << " | GPU compute\n";

  // ── Serial ──
  SerialPort serial;
  if (!serial.Open(opt.port, opt.baud)) {
    timeEndPeriod(1);
    return 1;
  }

  std::cout << "Connected to " << opt.port << " @ " << opt.baud << "\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(AppConfig::kSerialBootWaitMs));

  // ── Packet buffer ──
  std::array<uint8_t, PKT> packet{};
  auto hdr = BuildAdalightHeader();
  std::copy(hdr.begin(), hdr.end(), packet.begin());
  uint8_t* payload = packet.data() + 6;

  // ── State (flat arrays, cache-friendly) ──
  alignas(16) uint8_t curRgb[DATA_SIZE]{};
  alignas(16) uint8_t prevRgb[DATA_SIZE]{};
  alignas(16) float smoothBuf[DATA_SIZE]{};
  const bool doSmooth = opt.smoothing < 1.0;
  const float sAlpha = static_cast<float>(opt.smoothing);
  const float sKeep = 1.0f - sAlpha;
  bool smoothInit = false;

  // ── High-resolution waitable timer for sub-ms frame pacing ──
  HANDLE frameTimer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!frameTimer)
    frameTimer = CreateWaitableTimer(nullptr, TRUE, nullptr);

  const auto frameDur = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / opt.fps));
  const auto lowPowerFrameDur = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / AppConfig::kLowPowerCaptureFps));
  auto lastPrint = std::chrono::steady_clock::now();
  int capturedFrames = 0;
  int wakeCycles = 0;
  double pGpu = 0, pProcess = 0, pSerial = 0, pFrame = 0, pTargetFrameMs = 0;
  int pCount = 0;
  auto lastWrite = std::chrono::steady_clock::now();
  auto lastVisualChange = std::chrono::steady_clock::now();
  auto nextCaptureDue = std::chrono::steady_clock::now();
  int noFrameBackoffMs = 1;
  bool lowPowerModeActive = false;
  int activeDownscale = opt.downscale;

  auto SleepFor = [&](std::chrono::steady_clock::duration d) {
    if (d <= std::chrono::steady_clock::duration::zero())
      return;
    if (frameTimer) {
      LARGE_INTEGER dueTime;
      dueTime.QuadPart = -static_cast<LONGLONG>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
      SetWaitableTimer(frameTimer, &dueTime, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(frameTimer, INFINITE);
    } else {
      std::this_thread::sleep_for(d);
    }
  };

  while (g_running.load(std::memory_order_relaxed)) {
    auto t0 = std::chrono::steady_clock::now();

    const auto lowPowerAfter = std::chrono::milliseconds(AppConfig::kLowPowerAfterMs);
    const bool lowPowerMode = (t0 - lastVisualChange) >= lowPowerAfter;
    const auto captureInterval = lowPowerMode ? lowPowerFrameDur : frameDur;
    const int targetEdgeDepth = opt.edgeDepth;
    const int targetDownscale = lowPowerMode
                                    ? std::max(opt.downscale, AppConfig::kLowPowerDownscale)
                                    : opt.downscale;

    if (lowPowerMode != lowPowerModeActive || targetDownscale != activeDownscale) {
      regions = BuildSampleRegions(W, H, targetEdgeDepth);
      if (!capture.InitCompute(regions.data(), static_cast<int>(regions.size()), targetDownscale)) {
        std::cerr << "Failed to reconfigure compute pipeline for low-power sampling settings\n";
        break;
      }
      lowPowerModeActive = lowPowerMode;
      activeDownscale = targetDownscale;
    }

    // Event-driven wake-up: no busy looping while waiting for next capture or keepalive.
    const auto keepAliveDue = lastWrite + std::chrono::milliseconds(AppConfig::kKeepAliveMs);
    const auto nextWorkDue = std::min(nextCaptureDue, keepAliveDue);
    if (t0 < nextWorkDue) {
      SleepFor(nextWorkDue - t0);
      continue;
    }

    ++wakeCycles;

    bool didCapture = false;
    auto g0 = std::chrono::steady_clock::now();
    auto g1 = g0;
    auto p0 = g0;
    auto p1 = g0;

    // ── GPU capture + compute (only when due) ──
    if (t0 >= nextCaptureDue) {
      int r = capture.GrabAndProcess(curRgb);
      if (r < 0)
        break;
      if (r == 1) {
        didCapture = true;
        g1 = std::chrono::steady_clock::now();
        noFrameBackoffMs = 1;

        // ── Smoothing (disabled in low-power mode) ──
        p0 = std::chrono::steady_clock::now();
        const bool doSmoothThisFrame = doSmooth && !lowPowerMode;
        if (doSmoothThisFrame) {
          if (!smoothInit) {
            for (int i = 0; i < DATA_SIZE; ++i)
              smoothBuf[i] = static_cast<float>(curRgb[i]);
            smoothInit = true;
          } else {
            for (int i = 0; i < DATA_SIZE; ++i)
              smoothBuf[i] = sKeep * smoothBuf[i] + sAlpha * static_cast<float>(curRgb[i]);
          }
          for (int i = 0; i < DATA_SIZE; ++i)
            curRgb[i] = static_cast<uint8_t>(std::clamp(smoothBuf[i], 0.0f, 255.0f));
        } else {
          // Force re-seed when smoothing turns back on after low-power mode.
          smoothInit = false;
        }
        p1 = std::chrono::steady_clock::now();

        // ── Overlay draw ──
        if (opt.visualize) {
          overlay.Draw(regions.data(), static_cast<int>(regions.size()), curRgb);
          // Pump window messages so the overlay stays responsive
          MSG msg;
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
        }

        nextCaptureDue = std::chrono::steady_clock::now() + captureInterval;
      } else {
        // No new desktop frame from DXGI: exponentially back off polling.
        const int intervalMs = std::max(1,
                                        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(captureInterval).count()));
        noFrameBackoffMs = std::min(noFrameBackoffMs * 2, intervalMs);
        nextCaptureDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(noFrameBackoffMs);
      }
    }

    // ── Serial write (changed frame, or keepalive once per second) ──
    auto w0 = std::chrono::steady_clock::now();
    const bool changed = didCapture && (std::memcmp(curRgb, prevRgb, DATA_SIZE) != 0);
    const double msSinceWrite = std::chrono::duration<double, std::milli>(w0 - lastWrite).count();
    if (changed || msSinceWrite >= AppConfig::kKeepAliveMs) {
      std::memcpy(payload, curRgb, DATA_SIZE);
      serial.Write(packet.data(), packet.size());
      std::memcpy(prevRgb, curRgb, DATA_SIZE);
      lastWrite = w0;
      if (changed)
        lastVisualChange = w0;
    }
    auto w1 = std::chrono::steady_clock::now();

    if (didCapture)
      ++capturedFrames;

    auto now = std::chrono::steady_clock::now();
    auto frameElapsed = now - t0;
    const double targetFrameMsThisWake = std::chrono::duration<double, std::milli>(captureInterval).count();
    double elapsed = std::chrono::duration<double>(now - lastPrint).count();

    if (opt.profile && didCapture) {
      ++pCount;
      pGpu += std::chrono::duration<double, std::milli>(g1 - g0).count();
      pProcess += std::chrono::duration<double, std::milli>(p1 - p0).count();
      pSerial += std::chrono::duration<double, std::milli>(w1 - w0).count();
      pFrame += std::chrono::duration<double, std::milli>(frameElapsed).count();
      pTargetFrameMs += targetFrameMsThisWake;
    }

    if (elapsed >= AppConfig::kStatusPrintIntervalSec) {
      const double capFps = capturedFrames / elapsed;
      const double wakeFps = wakeCycles / elapsed;
      const char* modeName = lowPowerMode ? "LOW_POWER" : "ACTIVE";
      if (opt.profile && pCount) {
        const double avgGpu = pGpu / pCount;
        const double avgProcess = pProcess / pCount;
        const double avgSerial = pSerial / pCount;
        const double avgFrame = pFrame / pCount;
        const double budgetPct = pTargetFrameMs > 0.0 ? (pFrame / pTargetFrameMs) * 100.0 : 0.0;
        const std::string status =
            std::string("fps(c/w): ") + FormatNumber(capFps, 2) + "/" + FormatNumber(wakeFps, 2) +
            " | gpu: " + FormatDuration(avgGpu) +
            " | proc: " + FormatDuration(avgProcess) +
            " | serial: " + FormatDuration(avgSerial) +
            " | frame: " + FormatDuration(avgFrame) +
            " | mode: " + modeName +
            " | budget: " + FormatNumber(budgetPct, 1) + "%";
        PrintStatusLine(status);
        pGpu = pProcess = pSerial = pFrame = pTargetFrameMs = 0;
        pCount = 0;
      } else {
        const std::string status =
            std::string("fps(c/w): ") + FormatNumber(capFps, 2) + "/" + FormatNumber(wakeFps, 2) +
            " | mode: " + modeName;
        PrintStatusLine(status);
      }
      capturedFrames = 0;
      wakeCycles = 0;
      lastPrint = now;
    }
  }

  // Turn off LEDs
  std::memset(payload, 0, DATA_SIZE);
  serial.Write(packet.data(), packet.size());
  serial.Close();
  overlay.Destroy();
  capture.Cleanup();
  if (frameTimer)
    CloseHandle(frameTimer);
  timeEndPeriod(1);
  std::cout << "\nStopped.\n";
  return 0;
}
