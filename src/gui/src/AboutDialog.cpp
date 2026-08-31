/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AboutDialog.h"
#include "common/Version.h"

#include <QtCore>
#include <QtGui>

AboutDialog::AboutDialog(QWidget* parent, const QString& barrierApp) :
	QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
	Ui::AboutDialogBase()
{
	setupUi(this);

	m_versionChecker.setApp(barrierApp);
	const QString dataPlaneVersion = m_versionChecker.getVersion();
	m_pLabelBarrierVersion->setText(QString::fromLatin1(kBuildId));
	m_pLabelBarrierVersion->setToolTip(
		tr("Data process version: %1").arg(dataPlaneVersion));

	const QString buildDateText = QString::fromLatin1(kBuildDate);
	const QDate buildDate = QDate::fromString(buildDateText, "yyyyMMdd");
	m_pLabelBuildDate->setText(buildDate.isValid() ?
		QLocale().toString(buildDate, QLocale::LongFormat) : buildDateText);

	// change default size based on os
#if defined(Q_OS_MAC)
	QSize size(600, 380);
	setMaximumSize(size);
	setMinimumSize(size);
	resize(size);
#elif defined(Q_OS_LINUX)
	QSize size(600, 330);
	setMaximumSize(size);
	setMinimumSize(size);
	resize(size);
#endif
}
