/* OpenMT Radio: the Monitor tab, a virtual service monitor.  It draws what the signal monitor of the radio core makes of the samples
 * received from Modem_Host: the eye diagram, the histogram of the symbol values, the deviation and the spectrum of the
 * demodulated signal.  It only works while it is shown.
 *
 * The source of the eye, the histogram and the deviation is chosen at the top right: the IQ of the HackRF or SX1255 bridge (the
 * true signal: real Hz, the carrier's frequency error), the samples of the modem (what the decoder sees), or automatic (the IQ when
 * the bridge sends it and there is a signal in it). */

#ifndef OPENMT_MONITORWIDGET_H
#define OPENMT_MONITORWIDGET_H

#include <QWidget>

#include <vector>

#include "../core/iq_monitor.h"
#include "../core/rf_monitor.h"
#include "../core/rf_spectrum.h"

class QComboBox;
class QTimer;

class MonitorWidget : public QWidget
{
    Q_OBJECT

public:
    /* spectrum and iq may be null: then there is no RF spectrum, no waterfall and no IQ source */
    MonitorWidget(openmt::RfMonitor* monitor, openmt::SpectrumClient* spectrum, openmt::IqMonitor* iq, QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void refresh();
    void sourceChanged();

private:
    void drawPanel(QPainter& p, const QRect& r, const QString& title);
    void drawTrace(QPainter& p, const QRect& g, const std::vector<float>& db, double lo, double hi, const QColor& colour);

    void applySource();

    openmt::RfMonitor*      monitor_;
    openmt::SpectrumClient* spectrum_;
    openmt::IqMonitor*      iq_;
    QComboBox*              source_;
    openmt::MonitorSnapshot snap_;
    openmt::RfSpectrumSnapshot rf_;
    QTimer*                 timer_;
    bool                    iqAvailable_;
};

#endif
