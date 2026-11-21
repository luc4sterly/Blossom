#include <QSettings>
#include "blossom_qt/settings.hpp"

QString Settings::boot9_path;
QString Settings::boot11_path;
QString Settings::nand_path;
QString Settings::sd_path;
bool Settings::dev_unsigned = false;
QString Settings::cpu_backend = "jit";
QString Settings::cpu_backend_override;

namespace Settings
{

void load()
{
    QSettings qset("Blossom", "Blossom");

    boot9_path = qset.value("system/boot9", "").toString();
    boot11_path = qset.value("system/boot11", "").toString();
    nand_path = qset.value("system/nand", "").toString();
    sd_path = qset.value("system/sd", "").toString();
    cpu_backend = qset.value("cpu/backend", "jit").toString();
}

void save()
{
    QSettings qset("Blossom", "Blossom");

    qset.setValue("system/boot9", boot9_path);
    qset.setValue("system/boot11", boot11_path);
    qset.setValue("system/nand", nand_path);
    qset.setValue("system/sd", sd_path);
    qset.setValue("cpu/backend", cpu_backend);
}

}
