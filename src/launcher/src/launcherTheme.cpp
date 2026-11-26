#include "launcherTheme.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QLocale>
#include <QObject>
#include <QPalette>
#include <QStyle>
#include <QVariant>
#include <QWidget>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
#if defined(_WIN32)
class SquareWindowCorners final: public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange) {
            auto* widget = qobject_cast<QWidget*>(object);
            if (widget != nullptr && widget->isWindow() && widget->internalWinId() != 0) {
                // Style only launcher-owned windows. Keep the system caption and resize behavior.
                using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
                static const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                static const auto set_attribute = dwm != nullptr
                    ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
                if (set_attribute != nullptr) {
                    constexpr DWORD corner_preference_attribute = 33;
                    constexpr DWORD do_not_round = 1;
                    const auto result = set_attribute(reinterpret_cast<HWND>(widget->internalWinId()),
                                                      corner_preference_attribute, &do_not_round,
                                                      sizeof(do_not_round));
                    widget->setProperty("squareWindowCornersApplied", SUCCEEDED(result));
                }
            }
        }
        return QObject::eventFilter(object, event);
    }
};
#endif
} // namespace

namespace LauncherTheme {
void Initialize(QApplication& application) {
    // Keep system colors and native window behavior; the shared stylesheet supplies flat geometry.
    application.setProperty("nativeLauncherStyle", application.style()->objectName());
    auto font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const auto available = QFontDatabase::families();
    const QStringList candidates = QLocale().language() == QLocale::Chinese
        ? QStringList {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
                       QStringLiteral("Segoe UI"), font.family()}
        : QStringList {QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"), font.family()};
    QStringList families;
    for (const auto& family : candidates) {
        if (available.contains(family, Qt::CaseInsensitive) && !families.contains(family)) {
            families.append(family);
        }
    }
    if (!families.isEmpty()) font.setFamilies(families);
    font.setPointSizeF(qMax(10.5, font.pointSizeF()));
    font.setWeight(QFont::Normal);
    font.setHintingPreference(QFont::PreferVerticalHinting);
    application.setFont(font);
#if defined(_WIN32)
    application.installEventFilter(new SquareWindowCorners(&application));
#endif
    QFile stylesheet(QStringLiteral(":/styles/launcher.qss"));
    if (stylesheet.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto palette = application.palette();
        const auto foreground = palette.color(QPalette::Active, QPalette::Text);
        const auto background = palette.color(QPalette::Active, QPalette::Window);
        const auto muted_channel = [](int text, int surface) {
            return qRound(text * 0.55 + surface * 0.45);
        };
        const QColor disabled_text(muted_channel(foreground.red(), background.red()),
                                   muted_channel(foreground.green(), background.green()),
                                   muted_channel(foreground.blue(), background.blue()));
        // Resolve these colors from the active application palette once. Native Windows
        // button palettes can otherwise substitute nearly black disabled/highlight text.
        auto rules = QString::fromUtf8(stylesheet.readAll());
        rules.replace(QStringLiteral("@accent-text@"),
                      palette.color(QPalette::Active, QPalette::HighlightedText).name());
        rules.replace(QStringLiteral("@disabled-text@"), disabled_text.name());
        const auto arrow_tone = foreground.lightness() > 128 ? QStringLiteral("light")
                                                           : QStringLiteral("dark");
        rules.replace(QStringLiteral("@arrow-down@"),
                      QStringLiteral(":/icons/chevron-down-%1.svg").arg(arrow_tone));
        rules.replace(QStringLiteral("@arrow-up@"),
                      QStringLiteral(":/icons/chevron-up-%1.svg").arg(arrow_tone));
        application.setStyleSheet(rules);
    }
}
} // namespace LauncherTheme
