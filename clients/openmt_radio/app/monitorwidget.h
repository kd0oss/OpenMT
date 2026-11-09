/* OpenMT Radio: the Monitor tab, a virtual service monitor.  It draws what the signal monitor of the radio core makes of the samples
 * received from Modem_Host: the eye diagram, the histogram of the symbol values, the deviation and the spectrum of the
 * demodulated signal.  It only works while it is shown. */

#ifndef OPENMT_MONITORWIDGET_H
#define OPENMT_MONITORWIDGET_H

#include <QWidget>

#include <vector>

#include "../core/rf_monitor.h"
#include "../core/rf_spectrum.h"

class QTimer;

class MonitorWidget : public QWidget
{
    Q_OBJECT

public:
    /* spectrum may be null: then there is no RF spectrum and no waterfall */
    MonitorWidget(openmt::RfMonitor* monitor, openmt::SpectrumClient* spectrum, QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void refresh();

private:
    void drawPanel(QPainter& p, const QRect& r, const QString& title);
    void drawTrace(QPainter& p, const QRect& g, const std::vector<float>& db, double lo, double hi, const QColor& colour);

    openmt::RfMonitor*      monitor_;
    openmt::SpectrumClient* spectrum_;
    openmt::MonitorSnapshot snap_;
    openmt::RfSpectrumSnapshot rf_;
    QTimer*                 timer_;
};

#endif
