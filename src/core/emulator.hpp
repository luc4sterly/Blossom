#ifndef EMULATOR_HPP
#define EMULATOR_HPP
#include <cstdint>
#include <atomic>
#include <memory>
#include <thread>
#include "core/arm9/aes.hpp"
#include "core/arm9/cartridge.hpp"
#include "core/arm9/dma9.hpp"
#include "core/arm9/emmc.hpp"
#include "core/arm9/interrupt9.hpp"
#include "core/arm9/rsa.hpp"
#include "core/arm9/sha.hpp"

#include "core/arm11/dsp.hpp"
#include "core/arm11/gpu.hpp"
#include "core/arm11/hash.hpp"
#include "core/arm11/mpcore_pmr.hpp"
#include "core/arm11/wifi.hpp"

#include "core/cpu/arm.hpp"
#include "core/cpu/cp15.hpp"
#include "core/cpu/mmu.hpp"
#include "core/cpu/vfp.hpp"

#include "core/i2c.hpp"
#include "core/pxi.hpp"
#include "core/scheduler.hpp"
#include "core/spi.hpp"
#include "core/timers.hpp"

namespace Dynarmic
{
    class ExclusiveMonitor;
}

class Emulator
{
    private:
        //ROMs
        uint8_t* otp;
        uint8_t otp_free[256], otp_locked[256];
        uint8_t boot9_free[1024 * 64], boot11_free[1024 * 64];
        uint8_t boot9_locked[1024 * 64], boot11_locked[1024 * 64];

        bool is_n3ds;
        int core_count;
        uint32_t clock_ctrl;
        uint32_t boot_overlay_addr;
        uint8_t boot_ctrl[4];

        uint32_t fcram_size;
        uint32_t arm9_ram_size;
        uint32_t qtm_size;

        uint8_t twl_consoleid[8];

        uint8_t* arm9_RAM;
        uint8_t* axi_RAM;
        uint8_t* fcram;
        uint8_t* dsp_mem;
        uint8_t* vram;
        uint8_t* qtm_ram;

        ARM_CPU arm9, arm11[4];
        CP15 arm9_cp15, arm11_cp15[4];
        MMU arm9_pu, arm11_mmu[4];
        VFP vfp[4];
        std::unique_ptr<Dynarmic::ExclusiveMonitor> exclusive_monitor;

        AES aes;
        Cartridge cartridge;
        Corelink_DMA cdma;
        DMA9 dma9;
        DSP dsp;
        EMMC emmc;
        GPU gpu;
        HASH hash;
        I2C i2c;
        Interrupt9 int9;
        MPCore_PMR mpcore_pmr;
        PXI pxi;
        RSA rsa;
        Scheduler scheduler;
        SHA sha;
        SPI spi;
        Timers timers;
        WiFi wifi;

        uint32_t config_bootenv;
        uint16_t config_cardselect;

        uint8_t config_cardctrl2;
        int card_reset;

        int frames;

        uint8_t dsp_mem_config[16];

        uint16_t HID_PAD;

        uint8_t sysprot9, sysprot11;
    public:
        Emulator();
        ~Emulator();

        void reset(bool cold_boot = true);
        void run();

        //CPU backends: the dynarmic JIT (default when built in) or Corgi's interpreter.
        //Takes effect immediately; call it before booting. Returns false if the JIT is not built in.
        bool set_cpu_backend(bool arm11_jit, bool arm9_jit);
        void pxi_sync_point(bool from_arm9);
        uint64_t memory_check_counter = 0;

        //Watchdog thread: reports where the emulator is when a frame takes too long or memory
        //grows past BLOSSOM_MAX_RAM_MB, and stops the process in the second case
        std::thread watchdog;
        std::atomic<bool> watchdog_stop{false};
        std::atomic<int64_t> last_frame_end_ms{0};
        void start_watchdog();
        void watchdog_loop();
        void watchdog_report(const char* reason, long mb);
        bool is_arm11_jit() const;
        bool is_arm9_jit() const;
        //Reads BLOSSOM_CPU (interp, jit, jit11 = JIT on ARM11 only) and applies it if set.
        void apply_cpu_backend_env();
        void print_state();
        void dump();
        void memdump11(int id, uint64_t start, uint64_t size);

        void load_roms(uint8_t* boot9, uint8_t* boot11);
        bool parse_essentials();
        bool mount_nand(std::string file_name);
        void set_dev_unsigned(bool enabled);
        bool mount_sd(std::string file_name);
        bool mount_cartridge(std::string file_name);

        //Test runner: loads an ELF at physical address 0 and runs it on the ARM9 (core 9) or on
        //ARM11 core 0 (core 11) with the MMU off. SWI $FF prints a character.
        void load_and_run_elf(uint8_t* elf, uint64_t size, int core = 9, bool thumb_entry = false,
                              uint64_t cycles = 1024 * 1024, int slice = 1024 * 4);

        uint8_t arm9_read8(uint32_t addr);
        uint16_t arm9_read16(uint32_t addr);
        uint32_t arm9_read32(uint32_t addr);
        void arm9_write8(uint32_t addr, uint8_t value);
        void arm9_write16(uint32_t addr, uint16_t value);
        void arm9_write32(uint32_t addr, uint32_t value);

        uint8_t arm11_read8(int core, uint32_t addr);
        uint16_t arm11_read16(int core, uint32_t addr);
        uint32_t arm11_read32(int core, uint32_t addr);
        void arm11_write8(int core, uint32_t addr, uint8_t value);
        void arm11_write16(int core, uint32_t addr, uint16_t value);
        void arm11_write32(int core, uint32_t addr, uint32_t value);

        void arm11_send_events(int id);

        uint8_t* get_top_buffer();
        uint8_t* get_bottom_buffer();
        void set_pad(uint16_t pad);

        void set_touchscreen(uint16_t x, uint16_t y);
        void clear_touchscreen();

        void power_button();
        void home_button(bool pressed);
};

#endif // EMULATOR_HPP
