#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include "blossom_qt/settings.hpp"
#include "blossom_qt/settingswindow.hpp"

SettingsWindow::SettingsWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(tr("Settings"));

    Settings::load();

    boot9_info = new QLabel(tr("No ROM selected"));
    boot11_info = new QLabel(tr("No ROM selected"));
    nand_info = new QLabel(tr("No image selected"));
    sd_info = new QLabel(tr("No image selected"));

    settings_update();

    QPushButton* boot9_button = new QPushButton(tr("Browse..."));
    QPushButton* boot11_button = new QPushButton(tr("Browse..."));
    QPushButton* nand_button = new QPushButton(tr("Browse..."));
    QPushButton* sd_button = new QPushButton(tr("Browse..."));

    connect(boot9_button, &QPushButton::pressed, this, [=, this]() {
        QString file_name = QFileDialog::getOpenFileName(this, tr("Open ARM9 boot ROM"), "", "Binary file (*.bin)");
        Settings::boot9_path = file_name;
        printf("Boot9: %s\n", Settings::boot9_path.toStdString().c_str());
        settings_update();
    });

    connect(boot11_button, &QPushButton::pressed, this, [=, this]() {
        QString file_name = QFileDialog::getOpenFileName(this, tr("Open ARM11 boot ROM"), "", "Binary file (*.bin)");
        Settings::boot11_path = file_name;
        settings_update();
    });

    connect(nand_button, &QPushButton::pressed, this, [=, this] {
        QString file_name = QFileDialog::getOpenFileName(this, tr("Open NAND Image"), "", "Binary file (*.bin)");
        Settings::nand_path = file_name;
        settings_update();
    });

    connect(sd_button, &QPushButton::pressed, this, [=, this] {
        QString file_name = QFileDialog::getOpenFileName(this, tr("Open SD Image"), "", "");
        Settings::sd_path = file_name;
        settings_update();
    });

    QGridLayout* grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("ARM9 boot ROM")), 0, 0);
    grid->addWidget(boot9_info, 0, 1);
    grid->addWidget(boot9_button, 0, 2);

    grid->addWidget(new QLabel(tr("ARM11 boot ROM")), 1, 0);
    grid->addWidget(boot11_info, 1, 1);
    grid->addWidget(boot11_button, 1, 2);

    grid->addWidget(new QLabel(tr("NAND")), 2, 0);
    grid->addWidget(nand_info, 2, 1);
    grid->addWidget(nand_button, 2, 2);

    grid->addWidget(new QLabel(tr("SD")), 3, 0);
    grid->addWidget(sd_info, 3, 1);
    grid->addWidget(sd_button, 3, 2);

    QGroupBox* system_files_box = new QGroupBox(tr("System Files"));
    system_files_box->setLayout(grid);

    cpu_box = new QComboBox;
    cpu_box->addItem(tr("JIT (dynarmic) - fast"), "jit");
    cpu_box->addItem(tr("JIT on ARM11, interpreter on ARM9"), "jit11");
    cpu_box->addItem(tr("Interpreter - slow, for debugging"), "interp");
    int cpu_index = cpu_box->findData(Settings::cpu_backend);
    cpu_box->setCurrentIndex(cpu_index >= 0 ? cpu_index : 0);
    connect(cpu_box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
    {
        Settings::cpu_backend = cpu_box->currentData().toString();
        Settings::save();
    });

    QGridLayout* cpu_grid = new QGridLayout;
    cpu_grid->addWidget(new QLabel(tr("CPU backend")), 0, 0);
    cpu_grid->addWidget(cpu_box, 0, 1);
    cpu_grid->addWidget(new QLabel(tr("Takes effect the next time the emulator boots.")), 1, 0, 1, 2);

    QGroupBox* cpu_settings_box = new QGroupBox(tr("CPU"));
    cpu_settings_box->setLayout(cpu_grid);

    QVBoxLayout* layout = new QVBoxLayout;
    layout->addWidget(system_files_box);
    layout->addWidget(cpu_settings_box);

    setLayout(layout);

    setMinimumWidth(400);
}

void SettingsWindow::settings_update()
{
    if (!Settings::boot9_path.isEmpty())
        boot9_info->setText(QFileInfo(Settings::boot9_path).fileName());
    else
        boot9_info->setText(tr("No ROM selected"));

    if (!Settings::boot11_path.isEmpty())
        boot11_info->setText(QFileInfo(Settings::boot11_path).fileName());
    else
        boot11_info->setText(tr("No ROM selected"));

    if (!Settings::nand_path.isEmpty())
        nand_info->setText(QFileInfo(Settings::nand_path).fileName());
    else
        nand_info->setText(tr("No image selected"));

    if (!Settings::sd_path.isEmpty())
        sd_info->setText(QFileInfo(Settings::sd_path).fileName());
    else
        sd_info->setText(tr("No image selected"));

    Settings::save();
}
