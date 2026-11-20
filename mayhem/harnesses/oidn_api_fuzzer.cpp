// Fuzz oidn's public C API image/buffer geometry: oidnNewBuffer, oidnWriteBuffer/oidnReadBuffer
// with fuzzed offsets, and oidnSetFilterImage with fuzzed format/width/height/byteOffset/
// pixelByteStride/rowByteStride, plus the filter parameters that drive tiling (maxMemoryMB,
// inputScale, hdr/srgb, quality), then commit + execute the RT filter. The library validates every
// image against the buffer it lives in, so a geometry it accepts must stay in bounds.
// The filter runs with the real rt_hdr_small weights embedded at build time (oidn_weights_blob).
#include <OpenImageDenoise/oidn.h>
#include <fuzzer/FuzzedDataProvider.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" const unsigned char oidn_weights_blob[];
extern "C" const size_t oidn_weights_blob_size;

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

// Mostly small values (so the filter actually runs), sometimes any size_t (overflow paths).
static size_t consumeSize(FuzzedDataProvider& fdp, size_t smallMax)
{
  switch (fdp.ConsumeIntegralInRange<int>(0, 3))
  {
  case 0:  return fdp.ConsumeIntegral<size_t>();
  case 1:  return 0;
  default: return fdp.ConsumeIntegralInRange<size_t>(0, smallMax);
  }
}

static const OIDNFormat formats[] = {
  OIDN_FORMAT_UNDEFINED,
  OIDN_FORMAT_FLOAT, OIDN_FORMAT_FLOAT2, OIDN_FORMAT_FLOAT3, OIDN_FORMAT_FLOAT4,
  OIDN_FORMAT_HALF,  OIDN_FORMAT_HALF2,  OIDN_FORMAT_HALF3,  OIDN_FORMAT_HALF4,
};

static void setImage(FuzzedDataProvider& fdp, OIDNFilter filter, const char* name, OIDNBuffer buf)
{
  const OIDNFormat format = formats[fdp.ConsumeIntegralInRange<size_t>(0, sizeof(formats) / sizeof(formats[0]) - 1)];
  const size_t width  = consumeSize(fdp, 64);
  const size_t height = consumeSize(fdp, 64);
  const size_t byteOffset = consumeSize(fdp, 256);
  const size_t pixelByteStride = consumeSize(fdp, 64);
  const size_t rowByteStride = consumeSize(fdp, 1024);
  oidnSetFilterImage(filter, name, buf, format, width, height, byteOffset, pixelByteStride, rowByteStride);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  FuzzedDataProvider fdp(data, size);
  const char* msg;

  // Buffers: the data buffer and (optionally) a separate output buffer.
  const size_t bufSize = fdp.ConsumeIntegralInRange<size_t>(0, 64 * 1024);
  OIDNBuffer buf = oidnNewBuffer(device, bufSize);
  OIDNBuffer outBuf = fdp.ConsumeBool() ? oidnNewBuffer(device, fdp.ConsumeIntegralInRange<size_t>(0, 64 * 1024)) : nullptr;

  // Fill through oidnWriteBuffer with a fuzzed offset; the byte count never exceeds our source.
  std::vector<uint8_t> src(bufSize);
  for (size_t i = 0; i < src.size(); ++i)
    src[i] = uint8_t((i * 131) ^ (i >> 7));
  const size_t wrOff = consumeSize(fdp, bufSize);
  const size_t wrLen = fdp.ConsumeIntegralInRange<size_t>(0, src.size());
  oidnWriteBuffer(buf, wrOff, wrLen, src.data());

  OIDNFilter filter = oidnNewFilter(device, fdp.ConsumeIntegralInRange<int>(0, 7) == 0 ? "RTLightmap" : "RT");
  oidnSetSharedFilterData(filter, "weights", (void*)oidn_weights_blob, oidn_weights_blob_size);

  setImage(fdp, filter, "color", buf);
  if (fdp.ConsumeIntegralInRange<int>(0, 3) == 0) setImage(fdp, filter, "albedo", buf);
  if (fdp.ConsumeIntegralInRange<int>(0, 3) == 0) setImage(fdp, filter, "normal", buf);
  setImage(fdp, filter, "output", outBuf ? outBuf : buf);

  oidnSetFilterBool(filter, "hdr", fdp.ConsumeBool());
  oidnSetFilterBool(filter, "srgb", fdp.ConsumeBool());
  oidnSetFilterBool(filter, "cleanAux", fdp.ConsumeBool());
  if (fdp.ConsumeBool())
    oidnSetFilterFloat(filter, "inputScale", fdp.ConsumeFloatingPoint<float>());
  if (fdp.ConsumeBool())
    oidnSetFilterInt(filter, "maxMemoryMB", fdp.ConsumeIntegralInRange<int>(-1, 64));
  if (fdp.ConsumeBool())
    oidnSetFilterInt(filter, "quality", fdp.ConsumeIntegral<int>());
  oidnGetDeviceError(device, &msg); // parameter errors are expected; keep going

  oidnCommitFilter(filter);
  if (oidnGetDeviceError(device, &msg) == OIDN_ERROR_NONE)
  {
    oidnExecuteFilter(filter);
    oidnGetDeviceError(device, &msg);
  }

  // Read back through oidnReadBuffer with a fuzzed offset; the byte count never exceeds our dest.
  std::vector<uint8_t> dst(bufSize);
  const size_t rdOff = consumeSize(fdp, bufSize);
  const size_t rdLen = fdp.ConsumeIntegralInRange<size_t>(0, dst.size());
  oidnReadBuffer(buf, rdOff, rdLen, dst.data());

  oidnReleaseFilter(filter);
  if (outBuf) oidnReleaseBuffer(outBuf);
  oidnReleaseBuffer(buf);
  oidnGetDeviceError(device, &msg); // reset the device error state for the next input
  return 0;
}
