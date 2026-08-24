#include "toml_document.h"
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QMap>
#include <QVariant>
#include <QDebug>

TomlDocument::~TomlDocument() = default;

bool TomlDocument::parse(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open Cargo.toml:" << filePath;
        return false;
    }

    m_rawText = QString::fromUtf8(file.readAll());
    file.close();

    // Split into lines for processing
    QStringList lines = m_rawText.split('\n', Qt::SkipEmptyParts);

    // Simple TOML parser: track nesting depth and build dotted paths
    int depth = 0;
    QString currentKey;
    QStringList keyStack;  // stack of key components

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();

        // Skip empty lines and comments
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        // Handle inline tables [key.subkey] = { a = 1 }
        if (line.startsWith('[')) {
            // Find the closing bracket
            int endBracket = line.indexOf(']');
            if (endBracket > 0) {
                QString section = line.mid(1, endBracket - 1).trimmed();
                // Split by dots for dotted path
                QStringList parts = section.split('.', Qt::SkipEmptyParts);
                keyStack.clear();
                for (const QString& part : parts) {
                    keyStack.append(part);
                }
                currentKey = keyStack.join('.');
                depth = keyStack.size();
            }
            continue;
        }

        // Handle array of tables [key.subkey.0]
        if (line.startsWith('[') && line.contains('.')) {
            int endBracket = line.indexOf(']');
            if (endBracket > 0) {
                QString section = line.mid(1, endBracket - 1).trimmed();
                QStringList parts = section.split('.', Qt::SkipEmptyParts);
                keyStack.clear();
                for (const QString& part : parts) {
                    keyStack.append(part);
                }
                currentKey = keyStack.join('.');
                depth = keyStack.size();
            }
            continue;
        }

        // Handle key = value pairs
        int eqPos = line.indexOf('=');
        if (eqPos > 0) {
            QString keyPart = line.left(eqPos).trimmed();
            QString valuePart = line.mid(eqPos + 1).trimmed();

            // Remove inline table brackets
            if (valuePart.startsWith('{') && valuePart.endsWith('}')) {
                valuePart = valuePart.mid(1, valuePart.size() - 2);
            }

            // Remove array brackets
            if (valuePart.startsWith('[') && valuePart.endsWith(']')) {
                valuePart = valuePart.mid(1, valuePart.size() - 2);
            }

            // Build dotted path
            QString fullPath = currentKey.isEmpty() ? keyPart : currentKey + '.' + keyPart;

            // Store the value
            m_paths.insert(fullPath, valuePart);
            m_keys.append(fullPath);

            // Handle nested values (e.g., dependencies.sqlx.version)
            QStringList nestedParts = keyPart.split('.', Qt::SkipEmptyParts);
            if (nestedParts.size() > 1) {
                QString parentPath = currentKey.isEmpty() ? nestedParts[0] : currentKey + '.' + nestedParts[0];
                QVariant parentVal = m_paths.value(parentPath, QVariant());
                if (parentVal.isValid()) {
                    m_nested.insert(parentPath, parentVal);
                }
            }
        }
    }

    return true;
}

QVariant TomlDocument::get(const QString& path) const
{
    if (m_paths.contains(path)) {
        return m_paths.value(path);
    }
    return QVariant();
}
