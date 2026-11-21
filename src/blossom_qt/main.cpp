#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include "blossom_qt/emuwindow.hpp"
#include "blossom_qt/settings.hpp"

using namespace std;

int main(int argc, char** argv)
{   
    QApplication a(argc, argv);
    QApplication::setApplicationName("Blossom");

    QCommandLineParser parser;
    parser.addHelpOption();

    parser.addOptions(
    {
        {"boot9","Path to the 64 KB ARM9 boot ROM.", "boot9"},
        {"boot11", "Path to the 64 KB ARM11 boot ROM.", "boot11"},
        {"nand", "NAND dump. Must be dumped from latest version of GodMode9.", "nand"},
        {"sd", "SD image dump. Optional, but required for sighaxed NANDs.", "sd"},
        {"autoload", "3DS cartridge. Starts emulation immediately.", "cart"},
        {"autoload-nocart", "Starts emulation immediately without a cartridge."},
        {"cpu", "CPU backend for this run: jit (default), jit11 (JIT on ARM11 only) or interp "
                "(interpreter, for debugging and comparing).", "backend"},
        {"dev-unsigned", "Development: accept the self-signed NCSD header of a synthetic NAND "
                         "built with tools/mknand. Real signatures are still checked."}
    });

    parser.process(a.arguments());

    QString boot9_path = parser.value("boot9");
    QString boot11_path = parser.value("boot11");
    QString nand_path = parser.value("nand");
    QString sd_path = parser.value("sd");

    Settings::load();

    if (!boot9_path.isEmpty())
        Settings::boot9_path = boot9_path;

    if (!boot11_path.isEmpty())
        Settings::boot11_path = boot11_path;

    if (!nand_path.isEmpty())
        Settings::nand_path = nand_path;

    if (!sd_path.isEmpty())
        Settings::sd_path = sd_path;

    Settings::dev_unsigned = parser.isSet("dev-unsigned");
    Settings::cpu_backend_override = parser.value("cpu");

    //The order of this is important - we need to save settings before EmuWindow is constructed.
    //Otherwise, the settings window will have the old settings in the UI.
    Settings::save();
    EmuWindow* emuwindow = new EmuWindow();

    QString cart_path = parser.value("autoload");
    if (parser.isSet("autoload-nocart"))
        emuwindow->boot_emulator("");
    else if (!cart_path.isEmpty())
        emuwindow->boot_emulator(cart_path);

    a.exec();

    return 0;
}
