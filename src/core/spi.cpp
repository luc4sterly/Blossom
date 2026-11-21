#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/spi.hpp"

#include "common/common.hpp"

SPI::SPI()
{

}

int SPI::get_bus(uint32_t addr)
{
    uint32_t bus_addr = addr & 0xFFFFFF00;
    if (bus_addr == 0x10142800)
        return 0;
    if (bus_addr == 0x10143000)
        return 1;
    return 2;
}

void SPI::reset()
{
    memset(nspi_cnt, 0, sizeof(nspi_cnt));
    memset(nspi_buff_pos, 0, sizeof(nspi_buff_pos));
    memset(nspi_cmd, 0, sizeof(nspi_cmd));
    memset(nspi_autopoll, 0, sizeof(nspi_autopoll));
    memset(nspi_int_mask, 0, sizeof(nspi_int_mask));
    memset(nspi_int_stat, 0, sizeof(nspi_int_stat));

    clear_touchscreen();

    //An erased flash. cfg finds no valid data and writes its defaults.
    memset(nor.data, 0xFF, sizeof(nor.data));
    nor.selected = false;
    nor.cmd = 0;
    nor.pos = 0;
    nor.addr = 0;
    nor.write_enabled = false;
    nor.commands = nor.writes = 0;
}

//BLOSSOM_TRACE_SPI=1: one stderr line per NOR flash command (capped), to see what cfg does with it
static bool trace_spi()
{
    static const bool enabled = [] {
        const char* env = getenv("BLOSSOM_TRACE_SPI");
        return env && env[0] && env[0] != '0';
    }();
    return enabled;
}

void SPI::nor_deselect()
{
    static int lines = 0;
    if (trace_spi() && nor.selected && lines < 3000)
    {
        lines++;
        fprintf(stderr, "[NOR] cmd $%02X bytes %d addr $%06X wel %d\n", nor.cmd, nor.pos,
                nor.addr & 0xFFFFFF, nor.write_enabled);
    }
    nor.selected = false;
    nor.cmd = 0;
    nor.pos = 0;
}

uint8_t SPI::nor_transfer(uint8_t in)
{
    if (!nor.selected)
    {
        nor.selected = true;
        nor.cmd = in;
        nor.pos = 0;
        nor.addr = 0;
        nor.commands++;
        switch (in)
        {
            case 0x06: //WREN
                nor.write_enabled = true;
                break;
            case 0x04: //WRDI
                nor.write_enabled = false;
                break;
        }
        return 0xFF;
    }

    int pos = nor.pos++;
    switch (nor.cmd)
    {
        case 0x05: //RDSR: bit 0 write in progress (writes finish at once), bit 1 write enable latch
            return nor.write_enabled ? 0x02 : 0x00;
        case 0x9F: //RDID: ST M45PE10
        {
            static const uint8_t id[3] = {0x20, 0x40, 0x11};
            return pos < 3 ? id[pos] : 0xFF;
        }
        case 0x03: //READ
        case 0x0B: //FAST READ (one dummy byte after the address)
        {
            if (pos < 3)
            {
                nor.addr = (nor.addr << 8) | in;
                return 0xFF;
            }
            if (nor.cmd == 0x0B && pos == 3)
                return 0xFF;
            uint8_t value = nor.data[nor.addr & (sizeof(nor.data) - 1)];
            nor.addr++;
            return value;
        }
        case 0x0A: //PW: page write (erase + program)
        case 0x02: //PP: page program (can only clear bits)
            if (pos < 3)
            {
                nor.addr = (nor.addr << 8) | in;
                return 0xFF;
            }
            if (nor.write_enabled)
            {
                //Addresses wrap inside the 256-byte page
                uint32_t page = nor.addr & ~0xFFu;
                uint32_t a = (page | ((nor.addr + pos - 3) & 0xFF)) & (sizeof(nor.data) - 1);
                if (nor.cmd == 0x0A)
                    nor.data[a] = in;
                else
                    nor.data[a] &= in;
                nor.writes++;
            }
            return 0xFF;
        case 0xDB: //PE: page erase
        case 0xD8: //SE: sector erase (64 KiB)
            if (pos < 3)
            {
                nor.addr = (nor.addr << 8) | in;
                if (pos == 2 && nor.write_enabled)
                {
                    uint32_t size = nor.cmd == 0xDB ? 0x100 : 0x10000;
                    uint32_t start = (nor.addr & ~(size - 1)) & (sizeof(nor.data) - 1);
                    memset(nor.data + start, 0xFF, size);
                    nor.write_enabled = false;
                }
            }
            return 0xFF;
        default:
            return 0xFF;
    }
}

uint32_t SPI::read32(uint32_t addr)
{
    int bus = get_bus(addr);
    int reg = (addr & 0xFF) >> 2;
    switch (reg)
    {
        case 0x00:
            printf("[SPI] Read NSPI_CNT%d\n", bus);
            return 0;
        case 0x03:
        {
            if (is_nor(bus, nspi_cnt[bus].device))
            {
                uint32_t value = 0;
                for (int i = 0; i < 4 && nspi_len[bus]; i++)
                {
                    value |= nor_transfer(0x00) << (i * 8);
                    nspi_len[bus]--;
                }
                return value;
            }
            uint32_t value = nspi_buff[bus][nspi_buff_pos[bus]];
            nspi_buff_pos[bus]++;
            nspi_len[bus] -= 4;

            if (!nspi_len[bus])
                nspi_buff_pos[bus] = 0;

            return value;
        }
        case 0x05:
            return nspi_autopoll[bus] & ~(1u << 31);
        case 0x07:
            return nspi_int_stat[bus];
        default:
            printf("[SPI] Unrecognized bus%d read32 $%08X\n", bus, addr);
            return 0;
    }
}

void SPI::write32(uint32_t addr, uint32_t value)
{
    int bus = get_bus(addr);
    int reg = (addr & 0xFF) >> 2;
    switch (reg)
    {
        case 0x00:
            printf("[SPI] Write NSPI_CNT%d: $%08X\n", bus, value);
            nspi_cnt[bus].bandwidth = value & 0x7;
            nspi_cnt[bus].device = (value >> 6) & 0x3;
            nspi_cnt[bus].busy = (value >> 15) & 0x1;
            break;
        case 0x01:
            printf("[SPI] Write NSPI_DONE%d: $%08X\n", bus, value);
            if (!(value & 0x1))
            {
                nspi_cmd[bus] = 0;
                if (bus == 2 && nor.selected)
                {
                    if (nor.cmd == 0x0A || nor.cmd == 0x02)
                        nor.write_enabled = false;
                    nor_deselect();
                }
            }
            break;
        case 0x02:
            printf("[SPI] Write NSPI_LEN%d: $%08X\n", bus, value);
            nspi_len[bus] = value;
            break;
        case 0x03:
            printf("[SPI] Write NSPI_FIFO%d: $%08X\n", bus, value);
            if (is_nor(bus, nspi_cnt[bus].device))
            {
                //A short transfer (len < 4) only sends its low bytes
                int count = nspi_len[bus] ? (nspi_len[bus] < 4 ? nspi_len[bus] : 4) : 4;
                for (int i = 0; i < count; i++)
                    nor_transfer((value >> (i * 8)) & 0xFF);
                nspi_len[bus] = nspi_len[bus] > (uint32_t)count ? nspi_len[bus] - count : 0;
                break;
            }
            if (!nspi_cmd[bus])
            {
                nspi_cmd[bus] = value;
                process_cmd(bus);
            }
            else
            {
                if (nspi_len[bus] >= 0x100)
                    EmuException::die("[SPI] Blocklen exceeds buffer size!");

                //nspi_buff[bus][nspi_buff_pos[bus]] = value;
                nspi_buff_pos[bus]++;

                nspi_len[bus] -= 4;
                if (!nspi_len[bus])
                {
                    nspi_buff_pos[bus] = 0;
                }
            }
            break;
        case 0x05:
            //AUTOPOLL: send a command byte and wait for one bit of the reply. NOR writes finish
            //at once here, so the poll completes immediately.
            nspi_autopoll[bus] = value;
            if ((value >> 31) && is_nor(bus, nspi_cnt[bus].device))
            {
                nor_deselect();
                nor_transfer(value & 0xFF);
                uint8_t reply = nor_transfer(0x00);
                nor_deselect();
                int bit = (value >> 24) & 0x7;
                bool want = (value >> 30) & 0x1;
                nspi_int_stat[bus] |= (((reply >> bit) & 0x1) == want) ? 0x2 : 0x4;
            }
            else if (value >> 31)
                nspi_int_stat[bus] |= 0x2;
            break;
        case 0x06:
            nspi_int_mask[bus] = value;
            break;
        case 0x07:
            nspi_int_stat[bus] &= ~value;
            break;
        default:
            printf("[SPI] Unrecognized bus%d write32 $%08X: $%08X\n", bus, addr, value);
    }
}

void SPI::process_cmd(int bus)
{
    int device = nspi_cnt[bus].device | (bus << 4);

    memset(nspi_buff[bus], 0, sizeof(nspi_buff[bus]));
    nspi_buff_pos[bus] = 0;

    uint8_t temp_buff[0x34];

    switch (device)
    {
        //CODEC
        case 0x00:
            switch (nspi_cmd[bus])
            {
                case 0x03:
                    //Read input info

                    //Touchscreen
                    for (int i = 0; i < 5; i++)
                    {
                        *(uint16_t*)&temp_buff[i << 1] = touchscreen_x;
                        *(uint16_t*)&temp_buff[(i << 1) + 10] = touchscreen_y;
                    }

                    //Circle pad positions
                    for (int i = 0; i < 4; i++)
                    {
                        *(uint32_t*)&temp_buff[(i << 2) + 0x14] = 0x80008;
                        *(uint32_t*)&temp_buff[(i << 2) + 0x24] = 0x80008;
                    }

                    memcpy(nspi_buff[bus], temp_buff, 0x34);
                    break;
                default:
                    printf("[CODEC] Unrecognized command $%02X\n", nspi_cmd[bus]);
            }
            break;
        default:
            printf("[SPI] Unrecognized device $%02X\n", device);
    }
}

void SPI::set_touchscreen(uint16_t x, uint16_t y)
{
    //We need to scale from pixel coordinates (on a 320x240 screen) to touchscreen coordinates (4096x4096)
    touchscreen_x = x * 4096 / 320;
    touchscreen_y = y * 4096 / 240;

    touchscreen_x = bswp16(touchscreen_x);
    touchscreen_y = bswp16(touchscreen_y);

    printf("TOUCH! %d %d\n", touchscreen_x, touchscreen_y);
}

void SPI::clear_touchscreen()
{
    touchscreen_x = 0xFFFF;
    touchscreen_y = 0xFFFF;
}
