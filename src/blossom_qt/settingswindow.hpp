#ifndef SETTINGSWINDOW_H
#define SETTINGSWINDOW_H

#include <QComboBox>
#include <QLabel>
#include <QWidget>

class SettingsWindow : public QWidget
{
    Q_OBJECT
    private:
        QLabel* boot9_info;
        QLabel* boot11_info;
        QLabel* nand_info;
        QLabel* sd_info;
        QComboBox* cpu_box;
        QComboBox* gpu_box;

        void settings_update();
    public:
        explicit SettingsWindow(QWidget *parent = nullptr);
    signals:

    public slots:
};

#endif // SETTINGSWINDOW_H
