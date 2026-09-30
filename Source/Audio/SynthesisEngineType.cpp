#include "SynthesisEngineType.h"

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi1_2.h>
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "dxgi.lib")
#endif
#endif

namespace
{
#ifdef _WIN32
/**
 * True when the adapter is an integrated GPU, i.e. D3D12 reports it as a UMA
 * (unified memory) architecture. d3d12.dll is loaded at runtime rather than
 * linked, so a machine without it simply reports "not integrated" and the
 * caller's other checks decide.
 *
 * Returns false via @p ok when the question could not be answered at all
 * (no D3D12 runtime, or the adapter cannot create a device) - an adapter
 * DirectML itself could not use.
 */
bool isIntegratedAdapter(IDXGIAdapter1 *adapter, bool &ok)
{
  ok = false;

  static const auto createDevice = []() -> PFN_D3D12_CREATE_DEVICE
  {
    if (auto *module = LoadLibraryW(L"d3d12.dll"))
      return reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
          GetProcAddress(module, "D3D12CreateDevice"));
    return nullptr;
  }();

  if (createDevice == nullptr)
    return false;

  ID3D12Device *device = nullptr;
  if (FAILED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                          __uuidof(ID3D12Device),
                          reinterpret_cast<void **>(&device))) ||
      device == nullptr)
    return false;

  D3D12_FEATURE_DATA_ARCHITECTURE arch{};
  arch.NodeIndex = 0;
  const bool gotArch = SUCCEEDED(device->CheckFeatureSupport(
      D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)));
  device->Release();

  if (!gotArch)
    return false;

  ok = true;
  return arch.UMA != FALSE;
}

/**
 * Whether a fresh Windows install should start on the vocoder: at least one
 * hardware adapter, and not a lone integrated GPU.
 */
bool windowsHasVocoderCapableGpu()
{
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void **>(&factory))) ||
      factory == nullptr)
    return false;

  int hardwareAdapters = 0;
  bool loneAdapterIsIntegrated = false;

  for (UINT i = 0;; ++i)
  {
    IDXGIAdapter1 *adapter = nullptr;
    const auto hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr) || adapter == nullptr)
      continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
    {
      ++hardwareAdapters;
      if (hardwareAdapters == 1)
      {
        bool ok = false;
        const bool integrated = isIntegratedAdapter(adapter, ok);
        // An adapter D3D12 cannot open is no better for the vocoder than an
        // integrated one: DirectML would fall back to the CPU either way.
        loneAdapterIsIntegrated = !ok || integrated;
      }
    }
    adapter->Release();

    if (hardwareAdapters > 1)
      break;
  }

  factory->Release();

  if (hardwareAdapters == 0)
    return false;
  if (hardwareAdapters == 1 && loneAdapterIsIntegrated)
    return false;
  return true;
}
#endif
} // namespace

SynthesisEngineType defaultSynthesisEngineType()
{
#if JUCE_LINUX
  return SynthesisEngineType::Psola;
#elif defined(_WIN32)
  static const SynthesisEngineType cached =
      windowsHasVocoderCapableGpu() ? SynthesisEngineType::Vocoder
                                    : SynthesisEngineType::Psola;
  return cached;
#else
  return SynthesisEngineType::Vocoder;
#endif
}
