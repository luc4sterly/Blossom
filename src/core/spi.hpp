#ifndef SPI_HPP
#define SPI_HPP
#include <cstdint>

struct NSPI_CNT
{
    uint8_t bandwidth;
    uint8_t device;
    bool busy;
};

class SPI
{
    private:
        int get_bus(uint32_t addr);

        NSPI_CNT nspi_cnt[3];
        int nspi_buff_pos[3];
        uint32_t nspi_len[3];
        uint32_t nspi_buff[3][0x100];
        uint32_t nspi_cmd[3];
        uint32_t nspi_autopoll[3], nspi_int_mask[3], nspi_int_stat[3];

        uint16_t touchscreen_x, touchscreen_y;

        //WiFi/NVRAM NOR flash (bus at 0x10160000, device 1): 128 KiB, ST M45PE10-style command set.
        //Driven one byte at a time while chip select is held, i.e. until NSPI_DONE is cleared.
        struct NorFlash
        {
            uint8_t data[0x20000];
            bool selected;
            uint8_t cmd;
            int pos;          //bytes received since the command byte
            uint32_t addr;
            bool write_enabled;
            uint64_t commands, writes;
        } nor;
        uint8_t nor_transfer(uint8_t in);
        void nor_deselect();

        void process_cmd(int bus);
        static bool is_nor(int bus, int device) { return bus == 2 && device == 1; }
    public:
        SPI();

        void reset();

        uint32_t read32(uint32_t addr);
        void write32(uint32_t addr, uint32_t value);

        void set_touchscreen(uint16_t x, uint16_t y);
        void clear_touchscreen();
};

#endif // SPI_HPP
