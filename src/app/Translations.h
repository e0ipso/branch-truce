// Translations load the application's message catalogs for the user's language.
#pragma once

class QCoreApplication;

namespace branch_truce {
// Installs the English catalog, which supplies the plural forms of every
// numerus message, then the catalog of the user's preferred language over it
// when one is shipped. Returns whether the English catalog loaded.
bool installTranslations(QCoreApplication &application);
}
