// Fuzz oidn's user-supplied weights blob (TZA): oidnSetSharedFilterData(filter, "weights", ...)
// -> commit (parseTZA, UNet graph construction, tensor reorder) -> execute (CPU conv/pool kernels)
// on a fixed tiny 8x8 HDR image. The input is the whole weights blob.
#include <OpenImageDenoise/oidn.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

static OIDNDevice device;

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
  device = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
  oidnSetDeviceInt(device, "numThreads", 1);
  oidnSetDeviceBool(device, "setAffinity", false);
  oidnCommitDevice(device);
  const char* msg;
  if (oidnGetDeviceError(device, &msg) != OIDN_ERROR_NONE)
    abort();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const size_t W = 8, H = 8, pixBytes = 3 * sizeof(float);
  // Fixed input image (same every iteration).
  std::vector<float> pixels(W * H * 3);
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = float((i * 37) % 101) / 50.f;

  // The library keeps pointers into the shared blob until the filter is released: give it an
  // exact-size heap copy so ASan sees any read past the end.
  uint8_t* blob = (uint8_t*)malloc(size ? size : 1);
  if (size)
    memcpy(blob, data, size);

  OIDNBuffer colorBuf = oidnNewBuffer(device, W * H * pixBytes);
  OIDNBuffer outputBuf = oidnNewBuffer(device, W * H * pixBytes);
  oidnWriteBuffer(colorBuf, 0, W * H * pixBytes, pixels.data());

  OIDNFilter filter = oidnNewFilter(device, "RT");
  oidnSetFilterImage(filter, "color", colorBuf, OIDN_FORMAT_FLOAT3, W, H, 0, 0, 0);
  oidnSetFilterImage(filter, "output", outputBuf, OIDN_FORMAT_FLOAT3, W, H, 0, 0, 0);
  oidnSetFilterBool(filter, "hdr", true);
  oidnSetSharedFilterData(filter, "weights", blob, size);
  oidnCommitFilter(filter);

  const char* msg;
  if (oidnGetDeviceError(device, &msg) == OIDN_ERROR_NONE)
  {
    oidnExecuteFilter(filter);
    oidnGetDeviceError(device, &msg);
  }

  oidnReleaseFilter(filter);
  oidnReleaseBuffer(colorBuf);
  oidnReleaseBuffer(outputBuf);
  oidnGetDeviceError(device, &msg); // reset the device error state for the next input
  free(blob);
  return 0;
}
