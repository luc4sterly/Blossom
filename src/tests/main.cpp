#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include "core/emulator.hpp"
#include "common/exceptions.hpp"
#include "common/log.hpp"

using namespace std;

ifstream::pos_type filesize(const char* filename)
{
    ifstream in(filename, ifstream::ate | ifstream::binary);
    return in.tellg();
}

static int usage()
{
    printf("usage: blossom-cpu-tests [--cpu interp|jit] [--core arm9|arm11] [--thumb] [--cycles N] [--slice N] test.elf\n");
    return 1;
}

int main(int argc, char** argv)
{
    //The test ELFs print their results through SWI $FF, which goes to stdout
    Log::stdout_enabled = true;
    bool jit = false;
    int core = 9;
    bool thumb = false;
    uint64_t cycles = 1024 * 1024;
    int slice = 1024 * 4;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--cpu" && i + 1 < argc)
        {
            string v = argv[++i];
            if (v == "jit")
                jit = true;
            else if (v == "interp")
                jit = false;
            else
                return usage();
        }
        else if (arg == "--core" && i + 1 < argc)
        {
            string v = argv[++i];
            if (v == "arm9")
                core = 9;
            else if (v == "arm11")
                core = 11;
            else
                return usage();
        }
        else if (arg == "--cycles" && i + 1 < argc)
            cycles = std::stoull(argv[++i]);
        else if (arg == "--slice" && i + 1 < argc)
            slice = std::stoi(argv[++i]);
        else if (arg == "--thumb")
            thumb = true;
        else if (!path)
            path = argv[i];
        else
            return usage();
    }
    if (!path)
        return usage();

    ifstream test_elf_file(path, ios::binary);
    if (!test_elf_file.is_open())
        return 1;

    printf("Opened %s successfully, loading into memory\n", path);

    uint64_t size = filesize(path);
    uint8_t* mem = new uint8_t[size];

    test_elf_file.read((char*)mem, size);
    test_elf_file.close();

    Emulator e;
    if (jit && !e.set_cpu_backend(core == 11, core == 9))
    {
        printf("JIT requested but not built in\n");
        delete[] mem;
        return 1;
    }

    auto start = chrono::steady_clock::now();
    e.load_and_run_elf(mem, size, core, thumb, cycles, slice);
    double ms = chrono::duration<double, milli>(chrono::steady_clock::now() - start).count();
    printf("\n");
    if (cycles > 1024 * 1024)
        fprintf(stderr, "Ran %llu cycles in %.1f ms (%.1f MHz)\n", (unsigned long long)cycles, ms,
                cycles / ms / 1000.0);

    delete[] mem;

    return 0;
}
