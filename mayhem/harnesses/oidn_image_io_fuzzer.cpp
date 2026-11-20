// Fuzz the oidnDenoise apps' image loaders/savers (apps/utils/image_io.cpp): loadImage() on a
// .pfm or .phm file (PFM/PHM parsers, ImageBuffer allocation, sRGB conversion), then saveImage()
// of the loaded image back to the same format.
// Byte 0 selects the variant: bit0 = .phm instead of .pfm, bits1-2 = requested data type
// (undefined/float32/float16), bit3 = sRGB->linear conversion. The rest is the file content.
#include "apps/utils/image_io.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <unistd.h>

OIDN_NAMESPACE_USING

static DeviceRef* device;

static std::string tempPath(const char* ext)
{
  const char* tmp = getenv("TMPDIR");
  std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/oidn_fuzz_XXXXXX" + ext;
  int fd = mkstemps(&tmpl[0], (int)strlen(ext));
  if (fd < 0)
    abort();
  close(fd);
  return tmpl;
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
  device = new DeviceRef(newDevice(DeviceType::CPU));
  device->set("numThreads", 1);
  device->set("setAffinity", false);
  device->commit();
  const char* msg;
  if (device->getError(msg) != Error::None)
    abort();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size < 1)
    return -1;
  const uint8_t sel = data[0];
  const char* ext = (sel & 1) ? ".phm" : ".pfm";
  static const DataType types[4] = {DataType::Undefined, DataType::Float32, DataType::Float16, DataType::Undefined};
  const DataType dataType = types[(sel >> 1) & 3];
  const bool srgb = (sel >> 3) & 1;

  const std::string in = tempPath(ext);
  FILE* f = fopen(in.c_str(), "wb");
  if (!f)
    abort();
  if (size > 1)
    fwrite(data + 1, 1, size - 1, f);
  fclose(f);

  try
  {
    std::shared_ptr<ImageBuffer> image = loadImage(*device, in, srgb, dataType);
    if (image)
    {
      const std::string out = tempPath(ext);
      try
      {
        saveImage(out, *image, srgb);
      }
      catch (const std::exception&) {}
      unlink(out.c_str());
    }
  }
  catch (const std::exception&) {}

  unlink(in.c_str());
  const char* msg;
  device->getError(msg); // reset the device error state for the next input
  return 0;
}
