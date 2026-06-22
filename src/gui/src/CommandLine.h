#pragma once

#include <QString>
#include <QStringList>

namespace CommandLine {

QString quoteArgument(const QString& arg);
QString assembleCommand(const QString& app, const QStringList& args);

} // namespace CommandLine
