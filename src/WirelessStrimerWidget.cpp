/*---------------------------------------------------------*\
| WirelessStrimerWidget.cpp                                 |
|                                                           |
|   Plugin tab for configuring wireless Strimers            |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>
#include "WirelessStrimerManager.h"
#include "WirelessStrimerWidget.h"

enum
{
    COL_NAME,
    COL_MODEL,
    COL_ENABLED,
    COL_FLIP_X,
    COL_FLIP_Y,
    COL_LEDS,
    COL_MAC,
    COL_LINK,
    COL_STATUS,
    COL_IDENTIFY,
    COL_COUNT
};

static QTableWidgetItem* MakeCheckItem(bool checked)
{
    QTableWidgetItem* item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return(item);
}

static QTableWidgetItem* MakeTextItem(const QString& text)
{
    QTableWidgetItem* item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled);
    return(item);
}

WirelessStrimerWidget::WirelessStrimerWidget(WirelessStrimerManager* manager_ptr, QWidget* parent) : QWidget(parent)
{
    manager  = manager_ptr;
    updating = false;

    QVBoxLayout* layout = new QVBoxLayout(this);

    QLabel* intro = new QLabel(tr("Lian Li Strimer Wireless cables paired to the SL V3 wireless dongle. "
                                  "Pair new cables once in L-Connect 3, then quit L-Connect (including its service) "
                                  "so OpenRGB can use the dongle. Each enabled Strimer appears in the Devices tab."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    status_label = new QLabel();
    layout->addWidget(status_label);

    QHBoxLayout* controls = new QHBoxLayout();

    QPushButton* rescan = new QPushButton(tr("Rescan"));
    connect(rescan, &QPushButton::clicked, this, [this]() { manager->Rescan(); });
    controls->addWidget(rescan);

    controls->addWidget(new QLabel(tr("Max updates per second:")));
    fps_spin = new QSpinBox();
    fps_spin->setRange(1, 30);
    fps_spin->setToolTip(tr("Upper limit on color uploads per Strimer. Lower this if effects stutter or devices miss updates."));
    connect(fps_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value)
    {
        if(!updating)
        {
            manager->SetMaxFps((unsigned int)value);
        }
    });
    controls->addWidget(fps_spin);

    clock_check = new QCheckBox(tr("Send wireless clock heartbeat"));
    clock_check->setToolTip(tr("Sends the 1 Hz master clock frame L-Connect normally sends. Needed by wireless fans sharing this dongle."));
    connect(clock_check, &QCheckBox::toggled, this, [this](bool checked)
    {
        if(!updating)
        {
            manager->SetClockSync(checked);
        }
    });
    controls->addWidget(clock_check);
    controls->addStretch();
    layout->addLayout(controls);

    table = new QTableWidget(0, COL_COUNT);
    table->setHorizontalHeaderLabels({ tr("Name"), tr("Model"), tr("Enabled"), tr("Flip along cable"), tr("Flip lanes"),
                                       tr("LEDs"), tr("MAC"), tr("Channel / RX"), tr("Status"), QString() });
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    connect(table, &QTableWidget::cellChanged, this, &WirelessStrimerWidget::OnCellChanged);
    layout->addWidget(table, 1);

    other_label = new QLabel();
    other_label->setWordWrap(true);
    layout->addWidget(other_label);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &WirelessStrimerWidget::Refresh);
    timer->start(1000);

    Refresh();
}

void WirelessStrimerWidget::Refresh()
{
    ManagerStatus status = manager->GetStatus();

    updating = true;

    QString text = status.dongle_open ? tr("Dongle: connected") : tr("Dongle: not connected");
    if(!status.message.empty() && status.message != "Connected")
    {
        text += " - " + QString::fromStdString(status.message);
    }
    if(!status.master_mac.empty())
    {
        text += tr("  |  Master: %1").arg(QString::fromStdString(status.master_mac));
    }
    status_label->setText(text);

    if(!fps_spin->hasFocus())
    {
        fps_spin->setValue((int)status.max_fps);
    }
    clock_check->setChecked(status.clock_sync);

    /*-----------------------------------------------------*\
    | Rebuild rows only when the device set changes so that |
    | in-progress name edits are not clobbered              |
    \*-----------------------------------------------------*/
    std::vector<std::string> macs;
    for(const StrimerStatus& s : status.strimers)
    {
        macs.push_back(s.mac);
    }

    if(macs != row_macs)
    {
        row_macs = macs;
        table->setRowCount((int)status.strimers.size());

        for(int row = 0; row < (int)status.strimers.size(); row++)
        {
            const StrimerStatus& s = status.strimers[row];

            QTableWidgetItem* name = new QTableWidgetItem(QString::fromStdString(s.config.name));
            name->setToolTip(tr("Leave empty to use the model name"));
            table->setItem(row, COL_NAME,    name);
            table->setItem(row, COL_MODEL,   MakeTextItem(QString::fromStdString(s.model)));
            table->setItem(row, COL_ENABLED, MakeCheckItem(s.config.enabled));
            table->setItem(row, COL_FLIP_X,  MakeCheckItem(s.config.flip_x));
            table->setItem(row, COL_FLIP_Y,  MakeCheckItem(s.config.flip_y));
            table->setItem(row, COL_LEDS,    MakeTextItem(QString("%1 (%2 lanes)").arg(s.led_count).arg(s.lanes)));
            table->setItem(row, COL_MAC,     MakeTextItem(QString::fromStdString(s.mac)));
            table->setItem(row, COL_LINK,    MakeTextItem(QString()));
            table->setItem(row, COL_STATUS,  MakeTextItem(QString()));

            QPushButton* identify = new QPushButton(tr("Identify"));
            identify->setToolTip(tr("Light this Strimer white for 3 seconds"));
            std::string mac = s.mac;
            connect(identify, &QPushButton::clicked, this, [this, mac]() { manager->Identify(mac); });
            table->setCellWidget(row, COL_IDENTIFY, identify);
        }
    }

    for(int row = 0; row < (int)status.strimers.size(); row++)
    {
        const StrimerStatus& s = status.strimers[row];

        QString state;
        if(!s.config.enabled)
        {
            state = tr("Disabled");
        }
        else if(!s.seen)
        {
            state = tr("Not responding");
        }
        else if(s.uploads == 0)
        {
            state = tr("Waiting for colors");
        }
        else if(s.confirmed)
        {
            state = tr("OK");
        }
        else
        {
            state = tr("Sending");
        }
        state += tr(" (%1 uploads, %2 resends)").arg(s.uploads).arg(s.resends);

        table->item(row, COL_LINK)->setText(QString("%1 / %2").arg(s.channel).arg(s.rx_type));
        table->item(row, COL_STATUS)->setText(state);
    }

    if(status.other_devices.empty())
    {
        other_label->clear();
    }
    else
    {
        QStringList others;
        for(const std::string& other : status.other_devices)
        {
            others << QString::fromStdString(other);
        }
        other_label->setText(tr("Other wireless devices (not handled by this plugin): %1").arg(others.join(", ")));
    }

    updating = false;
}

void WirelessStrimerWidget::OnCellChanged(int row, int column)
{
    if(updating || row < 0 || row >= (int)row_macs.size())
    {
        return;
    }

    if(column == COL_NAME || column == COL_ENABLED || column == COL_FLIP_X || column == COL_FLIP_Y)
    {
        PushRowConfig(row);
    }
}

void WirelessStrimerWidget::PushRowConfig(int row)
{
    StrimerConfig config;

    config.name    = table->item(row, COL_NAME)->text().trimmed().toStdString();
    config.enabled = table->item(row, COL_ENABLED)->checkState() == Qt::Checked;
    config.flip_x  = table->item(row, COL_FLIP_X)->checkState()  == Qt::Checked;
    config.flip_y  = table->item(row, COL_FLIP_Y)->checkState()  == Qt::Checked;

    manager->SetStrimerConfig(row_macs[row], config);
}
