/*---------------------------------------------------------*\
| WirelessStrimerWidget.h                                   |
|                                                           |
|   Plugin tab for configuring wireless Strimers            |
|                                                           |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>
#include <string>
#include <vector>

class WirelessStrimerManager;

class WirelessStrimerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WirelessStrimerWidget(WirelessStrimerManager* manager, QWidget* parent = nullptr);

private slots:
    void    Refresh();
    void    OnCellChanged(int row, int column);

private:
    WirelessStrimerManager*     manager;
    QLabel*                     status_label;
    QLabel*                     other_label;
    QSpinBox*                   fps_spin;
    QCheckBox*                  clock_check;
    QTableWidget*               table;
    QTimer*                     timer;

    std::vector<std::string>    row_macs;
    bool                        updating;

    void    PushRowConfig(int row);
};
