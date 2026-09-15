/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "BarrierLocale.h"

#include <QResource>
#include <QXmlStreamReader>
#include <QDebug>
#include <QApplication>
#include <QTranslator>
#include <QPointer>

QString BarrierLocale::normalize(QString language)
{
    language = language.trimmed().section('.', 0, 0).section('@', 0, 0);
    language.replace('_', '-');
    if (language.startsWith("en", Qt::CaseInsensitive)) return "en";
    if (language.compare("zh", Qt::CaseInsensitive) == 0 ||
        language.compare("zh-Hans", Qt::CaseInsensitive) == 0 ||
        language.startsWith("zh-Hans-", Qt::CaseInsensitive) ||
        language.compare("zh-CN", Qt::CaseInsensitive) == 0 ||
        language.compare("zh-SG", Qt::CaseInsensitive) == 0) return "zh-CN";
    if (language.compare("zh-Hant", Qt::CaseInsensitive) == 0 ||
        language.startsWith("zh-Hant-", Qt::CaseInsensitive) ||
        language.compare("zh-TW", Qt::CaseInsensitive) == 0 ||
        language.compare("zh-HK", Qt::CaseInsensitive) == 0) return "zh-TW";
    BarrierLocale locales;
    for (const auto& candidate : locales.m_Languages)
        if (candidate.m_IetfCode.compare(language, Qt::CaseInsensitive) == 0)
            return candidate.m_IetfCode;
    return "en";
}

bool BarrierLocale::switchLanguage(const QString& language)
{
    static QPointer<QTranslator> current;
    const QString normalized = normalize(language);
    auto* next = new QTranslator(qApp);
    const bool loaded = normalized == "en" || next->load(":/res/lang/gui_" + normalized + ".qm");
    if (current) { qApp->removeTranslator(current); delete current; }
    current = nullptr;
    if (loaded && normalized != "en") {
        current = next;
        qApp->installTranslator(current);
    } else delete next;
    return loaded;
}

BarrierLocale::BarrierLocale()
{
    loadLanguages();
}

void BarrierLocale::loadLanguages()
{
    QResource resource(":/res/lang/Languages.xml");
    QByteArray bytes(reinterpret_cast<const char*>(resource.data()), resource.size());
    QXmlStreamReader xml(bytes);

    while (!xml.atEnd())
    {
        QXmlStreamReader::TokenType token = xml.readNext();
        if (xml.hasError())
        {
            qCritical() << xml.errorString();
            throw std::exception();
        }

        if (xml.name() == "language" && token == QXmlStreamReader::StartElement)
        {
            QXmlStreamAttributes attributes = xml.attributes();
            addLanguage(
                attributes.value("ietfCode").toString(),
                attributes.value("name").toString());
        }
    }
}

void BarrierLocale::addLanguage(const QString& ietfCode, const QString& name)
{
    m_Languages.push_back(BarrierLocale::Language(ietfCode, name));
}

void BarrierLocale::fillLanguageComboBox(QComboBox* comboBox)
{
    comboBox->blockSignals(true);
    QVector<BarrierLocale::Language>::iterator it;
    for (it = m_Languages.begin(); it != m_Languages.end(); ++it)
    {
        comboBox->addItem((*it).m_Name, (*it).m_IetfCode);
    }
    comboBox->blockSignals(false);
}
