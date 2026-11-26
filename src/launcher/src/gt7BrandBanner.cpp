#include "gt7BrandBanner.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>

void Gt7BrandBanner::SetArtwork(const QString& resource, double vertical_focus) {
	m_artwork.load(resource);
	m_vertical_focus = qBound(0.0, vertical_focus, 1.0);
	update();
}

void Gt7BrandBanner::paintEvent(QPaintEvent* event) {
	QFrame::paintEvent(event);
	QPainter painter(this);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.fillRect(rect(), QColor(6, 14, 25));
	if (!m_artwork.isNull() && width() > 0 && height() > 0) {
		const double scale = qMax(double(width()) / m_artwork.width(),
		                          double(height()) / m_artwork.height());
		const QSizeF visible(width() / scale, height() / scale);
		const QRectF source((m_artwork.width() - visible.width()) / 2.0,
		                    (m_artwork.height() - visible.height()) * m_vertical_focus,
		                    visible.width(), visible.height());
		painter.drawPixmap(QRectF(rect()), m_artwork, source);
	}
	// A quiet left-hand scrim keeps the authentic white wordmark readable in either
	// system theme without dimming the entire application or hiding the racing scene.
	QLinearGradient scrim(0, 0, width(), 0);
	scrim.setColorAt(0.0, QColor(3, 10, 22, 235));
	scrim.setColorAt(0.35, QColor(3, 10, 22, 195));
	scrim.setColorAt(0.68, QColor(3, 10, 22, 45));
	scrim.setColorAt(1.0, QColor(3, 10, 22, 12));
	painter.fillRect(rect(), scrim);
}
