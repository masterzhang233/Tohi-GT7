#ifndef GT7_BRAND_BANNER_H
#define GT7_BRAND_BANNER_H

#include <QFrame>
#include <QPixmap>

class QPaintEvent;

// A bounded photographic header. Original official assets stay unchanged on disk.
// All controls around the banner continue to use the Windows platform style.
class Gt7BrandBanner final: public QFrame {
public:
	explicit Gt7BrandBanner(QWidget* parent = nullptr): QFrame(parent) {}
	void SetArtwork(const QString& resource, double vertical_focus = 0.5);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	QPixmap m_artwork;
	double m_vertical_focus = 0.5;
};

#endif
