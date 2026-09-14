#include "ExternalScreenLayout.h"
#include <QHash>
#include <QPoint>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

ExternalScreenLayout::Result ExternalScreenLayout::parse(const QString& contents)
{
    Result result;
    if (contents.size() > maximumCharacters) return result;
    const QRegularExpression name("^[_A-Za-z0-9](?:[-_A-Za-z0-9]*[_A-Za-z0-9])?(?:\\.[_A-Za-z0-9](?:[-_A-Za-z0-9]*[_A-Za-z0-9])?)*\\.?$");
    const QRegularExpression sectionStart("^section:\\s*(screens|links|options|aliases)$");
    const QRegularExpression linkLine("^(left|right|up|down)\\s*=\\s*([^\\s()=]+)$");
    struct Edge { QString from, to; QPoint delta; };
    QVector<Edge> edges;
    QHash<QString, int> indices;
    QSet<QString> seenSections, occupiedSides;
    QString section, current;
    bool valid = true;
    for (QString line : contents.split('\n')) {
        line = line.section('#', 0, 0).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("section:")) {
            const auto match = sectionStart.match(line);
            if (!section.isEmpty() || !match.hasMatch()) { valid = false; continue; }
            section = match.captured(1);
            if (seenSections.contains(section)) valid = false;
            seenSections.insert(section);
            current.clear();
            continue;
        }
        if (line == "end") {
            if (section.isEmpty()) valid = false;
            section.clear(); current.clear(); continue;
        }
        if (section.isEmpty()) { valid = false; continue; }
        // These sections do not determine the displayed geometry. Alias targets
        // in links are deliberately unsupported rather than guessed.
        if (section == "options" || section == "aliases") continue;
        if (line.endsWith(':')) {
            current = line.left(line.size() - 1);
            if (!name.match(current).hasMatch()) { valid = false; current.clear(); continue; }
            if (section == "screens") {
                if (indices.contains(current)) { valid = false; continue; }
                if (result.devices.size() >= maximumScreens) return result;
                indices.insert(current, result.devices.size());
                result.devices.append({current, 0, 0});
            } else if (!indices.contains(current)) valid = false;
            continue;
        }
        if (current.isEmpty()) { valid = false; continue; }
        if (section == "screens") {
            // Screen options do not change adjacency; reject malformed lines.
            if (!line.contains('=')) valid = false;
            continue;
        }
        const auto match = linkLine.match(line);
        if (!match.hasMatch()) { valid = false; continue; }
        const QString side = match.captured(1), target = match.captured(2);
        const QString key = current + ':' + side;
        if (occupiedSides.contains(key) || !indices.contains(target)) { valid = false; continue; }
        occupiedSides.insert(key);
        const QPoint delta = side == "left" ? QPoint(-1, 0) : side == "right" ? QPoint(1, 0) :
                             side == "up" ? QPoint(0, -1) : QPoint(0, 1);
        edges.append({current, target, delta});
    }
    if (!valid || !section.isEmpty() || result.devices.isEmpty()) return result;
    // Treat each directed link as a positional constraint, not as evidence of
    // connectivity or a promise that the reverse handoff is configured.
    QHash<QString, QPoint> positions;
    positions.insert(result.devices.first().name, QPoint());
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& edge : edges) {
            if (positions.contains(edge.from)) {
                const QPoint next = positions.value(edge.from) + edge.delta;
                if (positions.contains(edge.to)) {
                    if (positions.value(edge.to) != next) return result;
                } else { positions.insert(edge.to, next); changed = true; }
            } else if (positions.contains(edge.to)) {
                positions.insert(edge.from, positions.value(edge.to) - edge.delta);
                changed = true;
            }
        }
    }
    if (positions.size() != result.devices.size()) return result;
    QSet<QString> cells;
    for (const auto& device : result.devices) {
        const QPoint point = positions.value(device.name);
        const QString cell = QString::number(point.x()) + ':' + QString::number(point.y());
        if (cells.contains(cell)) return result;
        cells.insert(cell);
    }
    for (auto& device : result.devices) {
        const QPoint point = positions.value(device.name);
        device.x = point.x(); device.y = point.y();
    }
    result.trustworthy = true;
    return result;
}
