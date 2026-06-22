#include "CommandLine.h"

#include <QChar>

namespace {

bool requiresQuoting(const QString& arg)
{
    if (arg.isEmpty()) {
        return true;
    }

    for (const QChar& ch : arg) {
        if (ch.isSpace()) {
            return true;
        }
    }

    return false;
}

} // namespace

namespace CommandLine {

QString quoteArgument(const QString& arg)
{
    if (!requiresQuoting(arg)) {
        return arg;
    }

    QString quoted = arg;
    quoted.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(quoted);
}

QString assembleCommand(const QString& app, const QStringList& args)
{
    QStringList command;
    command.reserve(args.size() + 1);
    command << quoteArgument(app);
    for (const QString& arg : args) {
        command << quoteArgument(arg);
    }
    return command.join(QStringLiteral(" "));
}

} // namespace CommandLine
