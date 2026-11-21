#include <fstream>

#include "blossom_qt/emuthread.hpp"
#include "blossom_qt/settings.hpp"

#include "common/exceptions.hpp"
#include "common/profiler.hpp"

using namespace std;

EmuThread::EmuThread()
{
    quit = true;
    has_frame_settings = false;
}

void EmuThread::run()
{

    while (!quit)
    {
        old_frametime = chrono::system_clock::now();
        {
            //Waiting for the UI to take the last frame shows up as "frontend" in BLOSSOM_PROFILE
            Profiler::Scope prof_scope(Profiler::FRONTEND);
            while (!has_frame_settings);
        }

        has_frame_settings = false;
        try
        {
            e.run();
        }
        catch (EmuException::FatalError& error)
        {
            printf("[EmuThread] Error occurred, stopping emulation...\n");
            printf("%s\n", error.what());
            e.print_state();
            fflush(stdout);
            emit emu_error(error.what());
            quit = true;
            break;
        }
        catch (EmuException::RebootException& r)
        {
            e.reset(false);
        }
        auto now = chrono::system_clock::now();
        auto elapsed_time = now - old_frametime;
        int milliseconds = chrono::duration_cast<chrono::milliseconds>(elapsed_time).count();
        Profiler::Scope prof_scope(Profiler::FRONTEND);
        emit frame_complete(e.get_top_buffer(), e.get_bottom_buffer(), milliseconds);
    }
}

//Returns true if all settings are parsed correctly, allowing the UI thread to start the emuthread.
bool EmuThread::boot_emulator(QString cart_path)
{
    //This function runs in the UI thread. Hence, it is safe for it to access Settings.
    quit = true;

    uint8_t boot9_rom[1024 * 64], boot11_rom[1024 * 64];

    ifstream boot9(Settings::boot9_path.toStdString());
    if (!boot9.is_open())
    {
        emit boot_error(tr("Failed to open ARM9 boot ROM."));
        return false;
    }

    boot9.read((char*)&boot9_rom, sizeof(boot9_rom));

    boot9.close();

    ifstream boot11(Settings::boot11_path.toStdString());
    if (!boot11.is_open())
    {
        emit boot_error(tr("Failed to open ARM11 boot ROM."));
        return false;
    }

    boot11.read((char*)&boot11_rom, sizeof(boot11_rom));

    boot11.close();

    e.load_roms(boot9_rom, boot11_rom);
    e.set_dev_unsigned(Settings::dev_unsigned);

    //CPU backend: saved setting, then --cpu, then BLOSSOM_CPU
    QString backend = Settings::cpu_backend_override.isEmpty() ? Settings::cpu_backend
                                                              : Settings::cpu_backend_override;
    if (backend == "interp")
        e.set_cpu_backend(false, false);
    else if (backend == "jit11")
        e.set_cpu_backend(true, false);
    else
        e.set_cpu_backend(true, true);
    e.apply_cpu_backend_env();

    //GPU renderer: saved setting, then --gpu, then BLOSSOM_GPU. Vulkan falls back to software.
    QString renderer = Settings::gpu_renderer_override.isEmpty() ? Settings::gpu_renderer
                                                                : Settings::gpu_renderer_override;
    e.set_gpu_renderer(renderer.toStdString().c_str());
    e.apply_gpu_renderer_env();

    if (!e.mount_nand(Settings::nand_path.toStdString()))
    {
        emit boot_error("Failed to load NAND image.");
        return false;
    }

    if (!Settings::sd_path.isEmpty())
    {
        if (!e.mount_sd(Settings::sd_path.toStdString()))
        {
            emit boot_error("Failed to load SD image.");
            return false;
        }
    }

    if (!cart_path.isEmpty())
    {
        if (!e.mount_cartridge(cart_path.toStdString()))
        {
            emit boot_error("Failed to load 3DS cartridge.");
            return false;
        }
    }

    if (!e.parse_essentials())
    {
        emit boot_error("Failed to find OTP and CID. Please make sure your NAND is dumped from the "
                        "latest version of GodMode9.");
        return false;
    }

    e.reset();

    quit = false;
    has_frame_settings = false;

    return true;
}

void EmuThread::pass_frame_settings(FrameSettings *f)
{
    e.set_pad(f->pad_state);

    if (f->touchscreen_pressed)
        e.set_touchscreen(f->touchscreen_x, f->touchscreen_y);
    else
        e.clear_touchscreen();

    if (f->power_button)
        e.power_button();

    if (f->home_button ^ f->old_home_button)
    {
        f->old_home_button = f->home_button;
        e.home_button(f->home_button);
    }

    has_frame_settings = true;
}
