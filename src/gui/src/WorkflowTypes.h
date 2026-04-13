#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>

enum class WorkflowRuntimeMode {
    Dormant,
    Active,
    Burst
};

enum class ContextKind {
    Unknown,
    Text,
    Link,
    Image,
    File,
    Folder
};

enum class SensitivityLevel {
    Normal,
    Sensitive
};

struct ActionDescriptor {
    QString id;
    QString label;
    QString riskLevel;
    bool requiresConfirmation = false;
    QVariantMap paramsSchema;
};

struct ContextItem {
    QString id;
    QString sourceDevice;
    ContextKind kind = ContextKind::Unknown;
    QString summary;
    qint64 byteSize = 0;
    QString previewRef;
    QString payloadRef;
    QString mimeType;
    SensitivityLevel sensitivity = SensitivityLevel::Normal;
    QStringList suggestedActions;
    QDateTime createdAt;
    QDateTime expiresAt;
    bool payloadAvailable = false;
    bool managedPayload = false;
    bool managedPreview = false;
};

struct SuggestionCard {
    QString id;
    QString contextId;
    QString source;
    QString targetDevice;
    QString actionId;
    QString previewText;
    double confidence = 0.0;
    QDateTime expiresAt;
    bool requiresConfirmation = false;
};

struct TransferReceipt {
    QString id;
    QString contextId;
    QString actionId;
    QString status;
    QString detail;
    QDateTime createdAt;
};

inline QString workflowRuntimeModeText(WorkflowRuntimeMode mode)
{
    switch (mode) {
    case WorkflowRuntimeMode::Dormant:
        return QStringLiteral("Dormant");
    case WorkflowRuntimeMode::Active:
        return QStringLiteral("Active");
    case WorkflowRuntimeMode::Burst:
        return QStringLiteral("Burst");
    }

    return QStringLiteral("Unknown");
}

inline QString contextKindText(ContextKind kind)
{
    switch (kind) {
    case ContextKind::Text:
        return QStringLiteral("Text");
    case ContextKind::Link:
        return QStringLiteral("Link");
    case ContextKind::Image:
        return QStringLiteral("Image");
    case ContextKind::File:
        return QStringLiteral("File");
    case ContextKind::Folder:
        return QStringLiteral("Folder");
    case ContextKind::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

inline QString sensitivityText(SensitivityLevel level)
{
    return level == SensitivityLevel::Sensitive
        ? QStringLiteral("Sensitive")
        : QStringLiteral("Normal");
}
