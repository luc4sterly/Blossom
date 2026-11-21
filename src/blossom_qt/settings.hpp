#ifndef SETTINGS_HPP
#define SETTINGS_HPP
#include <QString>

namespace Settings
{

//Boot settings - loaded once upon starting an emulator
extern QString boot9_path;
extern QString boot11_path;
extern QString nand_path;
extern QString sd_path;

//CPU backend: "jit" (dynarmic on ARM11 and ARM9), "jit11" (JIT on ARM11 only) or "interp".
//Saved; --cpu on the command line and the BLOSSOM_CPU environment variable override it for one run.
extern QString cpu_backend;
extern QString cpu_backend_override;

//GPU renderer: "vulkan" (default; falls back to software if Vulkan can't start) or "software".
//Saved; --gpu and BLOSSOM_GPU override it for one run.
extern QString gpu_renderer;
extern QString gpu_renderer_override;

//Development mode for synthetic NANDs (tools/mknand). Command line only, never saved.
extern bool dev_unsigned;

void load();
void save();

}

#endif // SETTINGS_HPP
