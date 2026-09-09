#include "GpuDeviceList.h"

#include <cstring>
#include <map>
#include <mutex>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
juce::StringArray getDxgiAdapterNames()
{
  juce::StringArray names;
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void **>(&factory))) ||
      factory == nullptr)
  {
    return names;
  }

  for (UINT i = 0;; ++i)
  {
    IDXGIAdapter1 *adapter = nullptr;
    const auto hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr) || adapter == nullptr)
      continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)))
    {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
      {
        names.add(juce::String(desc.Description));
      }
    }
    adapter->Release();
  }

  factory->Release();
  return names;
}
#endif

// The provider enumerators below only have a caller when ONNX Runtime is in
// the build; without it there are no execution providers to enumerate and they
// would compile to unused functions.
#ifdef HAVE_ONNXRUNTIME

juce::StringArray enumerateCudaDevices()
{
  juce::StringArray names;

#if defined(USE_CUDA) && defined(_WIN32)
  const char *cudaDllNames[] = {
      "cudart64_12.dll", // CUDA 12.x
      "cudart64_11.dll", // CUDA 11.x
      "cudart64_10.dll", // CUDA 10.x
      "cudart64.dll"     // Generic
  };

  HMODULE cudaLib = nullptr;
  for (const char *dllName : cudaDllNames)
  {
    cudaLib = LoadLibraryA(dllName);
    if (cudaLib)
      break;
  }

  if (cudaLib)
  {
    typedef int (*cudaGetDeviceCountFunc)(int *);
    typedef int (*cudaGetDevicePropertiesFunc)(void *, int);

    auto cudaGetDeviceCount =
        (cudaGetDeviceCountFunc)GetProcAddress(cudaLib, "cudaGetDeviceCount");

    if (cudaGetDeviceCount)
    {
      int deviceCount = 0;
      if (cudaGetDeviceCount(&deviceCount) == 0 && deviceCount > 0)
      {
        auto cudaGetDeviceProperties =
            (cudaGetDevicePropertiesFunc)GetProcAddress(
                cudaLib, "cudaGetDeviceProperties");

        for (int deviceId = 0; deviceId < deviceCount; ++deviceId)
        {
          juce::String deviceName = "GPU " + juce::String(deviceId);

          if (cudaGetDeviceProperties)
          {
            // cudaDeviceProp is large (~1KB) and we build without the CUDA
            // headers, so over-allocate. The name sits at the start of it.
            char propBuffer[2048];
            memset(propBuffer, 0, sizeof(propBuffer));

            if (cudaGetDeviceProperties(propBuffer, deviceId) == 0)
            {
              char *name = propBuffer;
              if (name[0] != '\0')
                deviceName = juce::String(name);
            }
          }

          names.add(deviceName + " (CUDA)");
        }
      }
    }
    FreeLibrary(cudaLib);
  }

  if (names.isEmpty())
  {
    // No CUDA runtime to ask, but DXGI still knows what hardware is present.
    for (const auto &name : getDxgiAdapterNames())
      names.add(name + " (DXGI)");
  }
#endif

  if (names.isEmpty())
    names.add("GPU 0 (CUDA)");

  return names;
}

juce::StringArray enumerateDirectMLDevices()
{
  juce::StringArray names;

#if defined(USE_DIRECTML) && defined(_WIN32)
  for (const auto &name : getDxgiAdapterNames())
    names.add(name + " (DirectML)");

  if (names.isEmpty())
  {
    // DirectML with no adapter list to go on: offer a small fixed range so a
    // machine we could not enumerate is still usable.
    for (int deviceId = 0; deviceId < 4; ++deviceId)
      names.add("GPU " + juce::String(deviceId) + " (DirectML)");
  }
#else
  // DirectML is not compiled in but the provider was reported as present.
  names.add("GPU 0 (DirectML)");
#endif

  return names;
}

#endif // HAVE_ONNXRUNTIME

juce::StringArray enumerateDevices(const juce::String &providerName)
{
#ifndef HAVE_ONNXRUNTIME
  juce::ignoreUnused(providerName);
  return {};
#else
  if (providerName == "CUDA")
    return enumerateCudaDevices();

  if (providerName == "DirectML")
    return enumerateDirectMLDevices();

  // CPU, and providers that pick their own device (CoreML, TensorRT): nothing
  // for the user to choose between.
  return {};
#endif
}

// Enumeration touches DXGI and the CUDA runtime, so do it once per provider.
// Adapters are fixed for the life of the process in every case that matters,
// and this is called from the message thread and from model reloads alike.
juce::StringArray getCachedDeviceNames(const juce::String &providerName)
{
  static std::mutex cacheMutex;
  static std::map<juce::String, juce::StringArray> cache;

  const std::lock_guard<std::mutex> lock(cacheMutex);
  const auto entry = cache.find(providerName);
  if (entry != cache.end())
    return entry->second;

  auto names = enumerateDevices(providerName);
  cache[providerName] = names;
  return names;
}
} // namespace

namespace GpuDeviceList
{
juce::StringArray getDeviceNames(const juce::String &providerName)
{
  return getCachedDeviceNames(providerName);
}

int getDefaultDeviceIndex(const juce::String &providerName)
{
  if (providerName != "DirectML")
    return 0;

  const auto names = getCachedDeviceNames(providerName);
  return names.size() > 1 ? names.size() - 1 : 0;
}

bool hasDeviceChoice(const juce::String &providerName)
{
  return getCachedDeviceNames(providerName).size() > 1;
}

juce::StringArray getDisplayAdapterNames()
{
#ifdef _WIN32
  return getDxgiAdapterNames();
#else
  return {};
#endif
}
} // namespace GpuDeviceList
