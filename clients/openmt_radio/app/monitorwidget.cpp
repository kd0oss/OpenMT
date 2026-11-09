#include "monitorwidget.h"

#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QTimer>

#include <algorithm>
#include <cmath>

MonitorWidget::MonitorWidget(openmt::RfMonitor* monitor, openmt::SpectrumClient* spectrum, QWidget* parent) :
    QWidget(parent),
    monitor_(monitor),
    spectrum_(spectrum),
    snap_(),
    rf_(),
    timer_(new QTimer(this))
{
    timer_->setInterval(150);
    QObject::connect(timer_, SIGNAL(timeout()), this, SLOT(refresh()));
    setAutoFillBackground(true);
    setFocusPolicy(Qt::NoFocus);                /* the space bar is the push to talk key */
}

QSize MonitorWidget::minimumSizeHint() const
{
    return QSize(720, 560);
}

void MonitorWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (spectrum_ != nullptr)
        spectrum_->setWanted(true);                 /* the HackRF bridge only computes the spectrum while it is asked */
    timer_->start();
}

void MonitorWidget::hideEvent(QHideEvent* event)
{
    timer_->stop();
    if (spectrum_ != nullptr)
        spectrum_->setWanted(false);
    QWidget::hideEvent(event);
}

void MonitorWidget::refresh()
{
    snap_ = monitor_->snapshot();
    if (spectrum_ != nullptr)
        rf_ = spectrum_->snapshot();
    update();
}

void MonitorWidget::drawPanel(QPainter& p, const QRect& r, const QString& title)
{
    p.setPen(QPen(QColor(70, 80, 90), 1));
    p.setBrush(QColor(12, 16, 20));
    p.drawRect(r);
    p.setPen(QColor(160, 170, 180));
    p.drawText(r.adjusted(6, 3, -6, -3), Qt::AlignTop | Qt::AlignLeft, title);
}

void MonitorWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(24, 28, 32));

    /* the RF line is long: it wraps, and the Signal panel grows to hold it */
    const QString rf = [this]() {
        QString rf;

        if (spectrum_ == nullptr)
        {
            rf = "RF: not available";
        }
        else if (!rf_.valid)
        {
            const std::string target = spectrum_->target();

            if (target.empty())
                rf = "RF: the spectrum is off (RF spectrum port 0, or not connected)";
            else
                rf = QString("RF: no answer from %1 (the HackRF and SX1255 bridges send the spectrum: check the bridge is the new build, "
                             "the port is the bridge's, and the host is right)").arg(QString::fromStdString(target));
        }
        else
        {
            const openmt::RfFrame& r = rf_.frame;

            rf = QString("RF: channel %1 dBFS   noise %2 dBFS/bin   signal-to-noise %3 dB   signal at %4 Hz from the centre %5 MHz   "
                         "ADC samples: peak %6 of 127, %7% at full scale")
                     .arg(r.channelDb, 0, 'f', 1).arg(r.noiseDb, 0, 'f', 1).arg(r.snrDb, 0, 'f', 0)
                     .arg(r.peakOffsetHz, 0, 'f', 0).arg(r.centreHz / 1e6, 0, 'f', 4)
                     .arg(r.adcPeak).arg(r.adcClipPercent, 0, 'f', 2);
            if (r.overload)
            {
                rf += "   OVERLOAD: lower the LNA and VGA gain of the bridge";
            }
        }
        return rf;
    }();
    const int rfWidth = std::max(100, rect().width() - 2 * 8 - 12);
    const int rfHeight = p.fontMetrics().boundingRect(QRect(0, 0, rfWidth, 1000), int(Qt::TextWordWrap), rf).height();

    const openmt::MonitorSnapshot& s = snap_;
    const int m = 8;
    const int readH = 88 + rfHeight + 8;
    QRect all = rect().adjusted(m, m, -m, -m);
    QRect readout(all.left(), all.top(), all.width(), readH);
    QRect body = all.adjusted(0, readH + m, 0, 0);
    const int row1 = body.height() * 38 / 100;
    const int row2 = body.height() * 30 / 100;
    const int row3 = body.height() - row1 - row2 - 2 * m;
    const int colW = body.width() / 2 - m / 2;
    QRect eyeR(body.left(), body.top(), colW, row1);
    QRect histR(body.left() + body.width() / 2 + m / 2, body.top(), colW, row1);
    QRect rfR(body.left(), body.top() + row1 + m, colW, row2);
    QRect specR(body.left() + body.width() / 2 + m / 2, body.top() + row1 + m, colW, row2);
    QRect fallR(body.left(), body.top() + row1 + row2 + 2 * m, body.width(), row3);

    /* readout */
    drawPanel(p, readout, "Signal");
    {
        QFont f = p.font();
        QString state;
        QColor col(200, 200, 200);

        if (s.samples == 0U)
        {
            state = "no samples yet: connect to a modem";
        }
        else if (!s.active)
        {
            state = "no signal";
        }
        else if (s.clipped)
        {
            state = "CLIPPING: the samples reach the limit of the modem, the measurements are not reliable";
            col = QColor(255, 90, 70);
        }
        else
        {
            state = "signal present";
            col = QColor(120, 220, 120);
        }

        p.setPen(col);
        p.drawText(readout.adjusted(6, 22, -6, 0), Qt::AlignTop | Qt::AlignLeft, state);
        p.setPen(QColor(220, 225, 230));

        QString l1 = QString("level: rms %1  peak %2 counts").arg(s.rms, 0, 'f', 0).arg(s.peak);
        QString l2;
        QString l3;

        if (s.levelsValid)
        {
            l2 = QString("%1 deviation of the outer symbols: %2 Hz = %3% of nominal%4  (nominal %5 Hz)")
                     .arg(s.approximate ? "~" : "=")
                     .arg(s.outerHz, 0, 'f', 0)
                     .arg(s.outerPercent, 0, 'f', 0)
                     .arg(s.approximate ? ", rough: the levels overlap" : "")
                     .arg(s.nominalHz, 0, 'f', 0);
            if (s.symbolLevels == 4U)
                l3 = QString("levels (counts): %1  %2  %3  %4    offset %5    spread %6    blur %7")
                         .arg(s.levels[0], 0, 'f', 0).arg(s.levels[1], 0, 'f', 0).arg(s.levels[2], 0, 'f', 0).arg(s.levels[3], 0, 'f', 0)
                         .arg(s.centre, 0, 'f', 0).arg(s.spread, 0, 'f', 0).arg(s.blur, 0, 'f', 2);
            else
                l3 = QString("levels (counts): %1  %2    offset %3    spread %4    blur %5")
                         .arg(s.levels[0], 0, 'f', 0).arg(s.levels[3], 0, 'f', 0)
                         .arg(s.centre, 0, 'f', 0).arg(s.spread, 0, 'f', 0).arg(s.blur, 0, 'f', 2);
        }
        else if (s.active && s.baud <= 0.0)
        {
            l2 = "this mode has no symbols: only the spectrum is shown";
        }
        p.drawText(readout.adjusted(6, 40, -6, 0), Qt::AlignTop | Qt::AlignLeft, l1);
        p.drawText(readout.adjusted(6, 56, -6, 0), Qt::AlignTop | Qt::AlignLeft, l2);
        p.drawText(readout.adjusted(6, 72, -6, 0), Qt::AlignTop | Qt::AlignLeft, l3);

        p.setPen(rf_.valid && rf_.frame.overload ? QColor(255, 90, 70) : QColor(220, 225, 230));
        p.drawText(readout.adjusted(6, 88, -6, 0), int(Qt::AlignTop | Qt::AlignLeft) | Qt::TextWordWrap, rf);
        (void)f;
    }

    /* eye */
    drawPanel(p, eyeR, "Eye (two symbols)");
    if (s.eyeTraces > 0U && s.eyePoints > 1U && s.levelsValid)
    {
        QRect g = eyeR.adjusted(8, 22, -8, -8);
        double top = std::max(1.0, std::fabs(s.histMax));
        p.setPen(QColor(50, 60, 70));
        p.drawLine(g.left(), g.center().y(), g.right(), g.center().y());
        p.drawLine(g.center().x(), g.top(), g.center().x(), g.bottom());
        /* the nominal levels, when known */
        if (s.nominalHz > 0.0)
        {
            double nom = s.nominalHz * openmt::RfMonitor::COUNTS_PER_HZ;

            p.setPen(QColor(80, 70, 30));
            for (int k = -1; k <= 1; k += 2)
            {
                int y = g.center().y() - (int)(k * nom / top * g.height() / 2.0);

                p.drawLine(g.left(), y, g.right(), y);
                if (s.symbolLevels == 4U)
                {
                    int y3 = g.center().y() - (int)(k * nom / 3.0 / top * g.height() / 2.0);

                    p.drawLine(g.left(), y3, g.right(), y3);
                }
            }
        }
        p.setPen(QColor(60, 220, 120, 70));
        for (unsigned t = 0U; t < s.eyeTraces; t++)
        {
            QPoint prev;

            for (unsigned j = 0U; j < s.eyePoints; j++)
            {
                double v = s.eye[(size_t)t * s.eyePoints + j];
                QPoint pt(g.left() + (int)((double)j / (double)(s.eyePoints - 1U) * g.width()),
                          g.center().y() - (int)(v / top * g.height() / 2.0));

                if (j > 0U)
                    p.drawLine(prev, pt);
                prev = pt;
            }
        }
    }

    /* histogram */
    drawPanel(p, histR, "Symbol values (yellow lines: nominal levels)");
    if (!s.hist.empty() && s.levelsValid)
    {
        QRect g = histR.adjusted(8, 22, -8, -18);
        uint32_t mx = *std::max_element(s.hist.begin(), s.hist.end());
        double bw = (double)g.width() / (double)s.hist.size();

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(90, 170, 255));
        for (size_t i = 0U; i < s.hist.size(); i++)
        {
            int h = mx > 0U ? (int)((double)s.hist[i] / (double)mx * g.height()) : 0;

            p.drawRect(QRectF(g.left() + (double)i * bw, g.bottom() - h, std::max(1.0, bw - 1.0), h));
        }
        if (s.nominalHz > 0.0)
        {
            double nom = s.nominalHz * openmt::RfMonitor::COUNTS_PER_HZ;
            double span = s.histMax - s.histMin;

            p.setPen(QColor(230, 200, 60));
            for (int k = -3; k <= 3; k++)
            {
                if (s.symbolLevels == 4U ? (k == 0 || k == -2 || k == 2) : (k != -3 && k != 3))
                    continue;
                double v = nom * k / 3.0;
                int x = g.left() + (int)((v - s.histMin) / span * g.width());

                p.drawLine(x, g.top(), x, g.bottom());
            }
        }
        p.setPen(QColor(160, 170, 180));
        p.drawText(QRect(g.left(), g.bottom() + 2, g.width(), 14), Qt::AlignLeft, QString("%1").arg(s.histMin, 0, 'f', 0));
        p.drawText(QRect(g.left(), g.bottom() + 2, g.width(), 14), Qt::AlignRight, QString("%1 counts").arg(s.histMax, 0, 'f', 0));
    }

    /* spectrum */
    drawPanel(p, specR, "Spectrum of the demodulated signal, 0 to 12 kHz (relative dB)");
    if (!s.spectrum.empty() && s.active)
    {
        QRect g = specR.adjusted(8, 22, -8, -18);
        float mx = *std::max_element(s.spectrum.begin(), s.spectrum.end());
        float lo = mx - 80.0F;
        QPoint prev;

        p.setPen(QColor(255, 170, 60));
        for (size_t i = 0U; i < s.spectrum.size(); i++)
        {
            double v = (s.spectrum[i] - lo) / 80.0;
            QPoint pt(g.left() + (int)((double)i / (double)(s.spectrum.size() - 1U) * g.width()),
                      g.bottom() - (int)(std::max(0.0, std::min(1.0, v)) * g.height()));

            if (i > 0U)
                p.drawLine(prev, pt);
            prev = pt;
        }
        p.setPen(QColor(160, 170, 180));
        for (int k = 0; k <= 12; k += 2)
        {
            int x = g.left() + (int)((double)k / 12.0 * g.width());

            p.drawText(QRect(x - 20, g.bottom() + 2, 40, 14), Qt::AlignHCenter, QString("%1k").arg(k));
        }
    }
    /* RF spectrum of the received channel (from the HackRF bridge) */
    drawPanel(p, rfR, "RF spectrum, 125 kHz around the channel (dBFS; lines: 12.5 kHz channel)");
    if (rf_.valid && !rf_.frame.db.empty())
    {
        QRect g = rfR.adjusted(8, 22, -8, -18);
        const openmt::RfFrame& r = rf_.frame;
        double lo = std::max(-110.0, r.noiseDb - 6.0);
        double hi = std::max(r.peakDb + 6.0, lo + 30.0);
        double span = (double)r.spanHz;

        for (int k = -1; k <= 1; k += 2)
        {
            int x = g.left() + (int)((0.5 + k * 6250.0 / span) * g.width());

            p.setPen(QColor(90, 80, 30));
            p.drawLine(x, g.top(), x, g.bottom());
        }
        drawTrace(p, g, r.db, lo, hi, QColor(120, 200, 255));
        p.setPen(QColor(160, 170, 180));
        for (int k = -50; k <= 50; k += 25)
        {
            int x = g.left() + (int)((0.5 + k * 1000.0 / span) * g.width());

            p.drawText(QRect(x - 24, g.bottom() + 2, 48, 14), Qt::AlignHCenter, QString("%1k").arg(k));
        }
        p.drawText(QRect(g.left(), g.top(), 80, 14), Qt::AlignLeft, QString("%1").arg(hi, 0, 'f', 0));
        p.drawText(QRect(g.left(), g.bottom() - 14, 80, 14), Qt::AlignLeft, QString("%1").arg(lo, 0, 'f', 0));
    }

    /* waterfall of the same, newest at the top */
    drawPanel(p, fallR, "Waterfall (newest at the top)");
    if (rf_.valid && !rf_.history.empty())
    {
        QRect g = fallR.adjusted(8, 22, -8, -8);
        const openmt::RfFrame& r = rf_.frame;
        double lo = std::max(-110.0, r.noiseDb - 6.0);
        double hi = std::max(r.peakDb + 6.0, lo + 30.0);
        size_t cols = rf_.history[0].size();
        QImage img((int)cols, (int)rf_.history.size(), QImage::Format_RGB32);

        for (size_t y = 0U; y < rf_.history.size(); y++)
        {
            const std::vector<uint8_t>& row = rf_.history[rf_.history.size() - 1U - y];

            for (size_t x = 0U; x < cols && x < row.size(); x++)
            {
                double v = (-0.5 * (double)row[x] - lo) / (hi - lo);

                v = std::max(0.0, std::min(1.0, v));
                /* black - blue - cyan - yellow - red */
                int rr = (int)(255.0 * std::max(0.0, std::min(1.0, (v - 0.5) * 4.0)));
                int gg = (int)(255.0 * std::max(0.0, std::min(1.0, (v - 0.25) * 4.0)));
                int bb = (int)(255.0 * std::max(0.0, std::min(1.0, v * 4.0)) * (v > 0.75 ? std::max(0.0, 1.0 - (v - 0.75) * 4.0) : 1.0));

                img.setPixel((int)x, (int)y, qRgb(rr, gg, bb));
            }
        }
        p.drawImage(g, img);
    }
}

void MonitorWidget::drawTrace(QPainter& p, const QRect& g, const std::vector<float>& db, double lo, double hi, const QColor& colour)
{
    QPoint prev;
    const size_t n = db.size();

    p.setPen(colour);
    for (size_t i = 0U; i < n; i++)
    {
        double v = (db[i] - lo) / (hi - lo);
        QPoint pt(g.left() + (int)((double)i / (double)(n - 1U) * g.width()),
                  g.bottom() - (int)(std::max(0.0, std::min(1.0, v)) * g.height()));

        if (i > 0U)
            p.drawLine(prev, pt);
        prev = pt;
    }
}
