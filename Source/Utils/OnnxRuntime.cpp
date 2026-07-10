#include "OnnxRuntime.h"

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

#if JUCE_MAC
#include <mutex>
#include <dlfcn.h>
#endif

namespace OnnxRuntime
{
namespace
{
#if JUCE_MAC
std::once_flag initialisationFlag;
bool initialised = false;
juce::String initialisationError;

const OrtApiBase *getApiBase()
{
  Dl_info imageInfo{};
  if (dladdr(reinterpret_cast<const void *>(&getApiBase), &imageInfo) == 0 ||
      imageInfo.dli_fname == nullptr)
  {
    initialisationError = "Could not locate the PitchNet plug-in bundle";
    return nullptr;
  }

  const auto plugInBinary = juce::File(imageInfo.dli_fname);
  const auto runtimePath = plugInBinary.getParentDirectory()
                               .getParentDirectory()
                               .getChildFile("Frameworks")
                               .getChildFile("libonnxruntime.dylib");

  void *runtime = dlopen(runtimePath.getFullPathName().toRawUTF8(),
                         RTLD_NOW | RTLD_LOCAL);
  if (runtime == nullptr)
  {
    initialisationError = "Could not load PitchNet's ONNX Runtime: " +
                          juce::String(dlerror());
    return nullptr;
  }

  using GetApiBaseFunction = const OrtApiBase *(*)();
  auto *getApiBase = reinterpret_cast<GetApiBaseFunction>(
      dlsym(runtime, "OrtGetApiBase"));
  if (getApiBase == nullptr)
  {
    initialisationError = "PitchNet's ONNX Runtime has no OrtGetApiBase symbol";
    return nullptr;
  }

  return getApiBase();
}
#endif
} // namespace

bool initialise(juce::String *errorMessage)
{
#if JUCE_MAC
  std::call_once(initialisationFlag, [] {
    const auto *apiBase = getApiBase();
    const auto *api = apiBase == nullptr ? nullptr : apiBase->GetApi(ORT_API_VERSION);

    if (api == nullptr)
    {
      if (initialisationError.isEmpty())
        initialisationError = "PitchNet's ONNX Runtime does not support API version " +
                              juce::String(ORT_API_VERSION);
      return;
    }

    Ort::InitApi(api);
    initialised = true;
  });

  if (!initialised && errorMessage != nullptr)
    *errorMessage = initialisationError;

  return initialised;
#else
  juce::ignoreUnused(errorMessage);
  return true;
#endif
}
} // namespace OnnxRuntime
#else
namespace OnnxRuntime
{
bool initialise(juce::String *) { return true; }
} // namespace OnnxRuntime
#endif
