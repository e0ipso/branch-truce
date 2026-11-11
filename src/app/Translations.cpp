// Translations load the application's message catalogs for the user's language.
#include "Translations.h"
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

namespace branch_truce {
bool installTranslations(QCoreApplication &application)
{
    const auto catalogDirectory = QStringLiteral(":/branch-truce/i18n");
    const auto catalogName = QStringLiteral("branch-truce");
    auto *english = new QTranslator(&application);
    const bool loaded = english->load(catalogName + QStringLiteral("_en"), catalogDirectory);
    if (loaded)
        QCoreApplication::installTranslator(english);
    else
        delete english;
    auto *preferred = new QTranslator(&application);
    // A catalog installed later wins, so the user's language overrides English where it has a translation.
    if (preferred->load(QLocale(), catalogName, QStringLiteral("_"), catalogDirectory)
        && preferred->language() != QLatin1String("en"))
        QCoreApplication::installTranslator(preferred);
    else
        delete preferred;
    return loaded;
}
}
