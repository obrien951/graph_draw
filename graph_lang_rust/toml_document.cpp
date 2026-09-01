#include "toml_document.h"

#include <QSet>
#include <stdexcept>

TomlDocument TomlDocument::parse(const QString& text)
{
    TomlDocument doc;
    doc.parseText(text);
    return doc;
}

void TomlDocument::parseText(const QString& text)
{
    // Parse TOML text into dotted paths, handling the requirements from the specification
    // - Parses to dotted paths: package.name, bin[1].name, dependencies.sqlx.version
    // - Malformed lines are skipped rather than fatal
    // - Multi-line """ strings must be skipped over without desynchronising tables
    // - Handles [section], [[array-of-tables]], dotted keys, quoted headers, etc.
    
    QStringList lines = text.split('\n');
    QString currentTable;
    bool inMultiLineString = false;
    
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        
        // Skip empty lines and comments
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        
        // Handle multi-line strings ("""...""") - skip over them completely
        if (line.startsWith("\"\"\"") && line.endsWith("\"\"\"")) {
            // Single line multi-line string - skip it
            continue;
        } else if (line.startsWith("\"\"\"")) {
            // Start of multi-line string - mark and skip
            inMultiLineString = true;
            continue;
        } else if (inMultiLineString) {
            // Continue skipping multi-line string content
            if (line.endsWith("\"\"\"")) {
                // End of multi-line string
                inMultiLineString = false;
            }
            continue;
        } else if (line.endsWith("\"\"\"")) {
            // This shouldn't happen if our logic is correct, but just in case
            continue;
        }
        
        // Handle table headers [section] and [[array-of-tables]]
        if (line.startsWith('[') && line.endsWith(']')) {
            if (line.startsWith("[[")) {
                // Array of tables [[table]]
                currentTable = line.mid(2, line.length() - 4);
                // Remove any trailing ] from array tables
                if (currentTable.endsWith(']')) {
                    currentTable = currentTable.left(currentTable.length() - 1);
                }
                // Increment the count for this array table
                m_arrayTables[currentTable]++;
            } else {
                // Simple table [table]
                currentTable = line.mid(1, line.length() - 2);
            }
            continue;
        }
        
        // Handle key-value pairs
        int equalsPos = line.indexOf('=');
        if (equalsPos != -1) {
            QString key = line.left(equalsPos).trimmed();
            QString value = line.mid(equalsPos + 1).trimmed();
            
            // Skip malformed entries
            if (key.isEmpty() || value.isEmpty()) {
                continue;
            }
            
            // Handle quoted keys that might have dots
            if (key.startsWith('"') && key.endsWith('"')) {
                // Remove quotes - this preserves dotted keys like "target.'cfg(target_arch = "wasm32")'.dependencies"
                key = key.mid(1, key.length() - 2);
            }
            
            // Build the full dotted path
            QString fullPath;
            if (currentTable.isEmpty()) {
                fullPath = key;
            } else {
                fullPath = currentTable + '.' + key;
            }
            
            // Skip malformed entries
            if (!fullPath.isEmpty()) {
                Entry entry;
                entry.path = fullPath;
                entry.rawValue = value;
                m_entries.append(entry);
            }
        }
    }
}

QStringList TomlDocument::keys(const QString& prefix) const
{
    QStringList result;
    QSet<QString> seen;
    
    for (const Entry& entry : m_entries) {
        QString path = entry.path;
        
        // If we have a prefix, only consider paths that start with it
        if (!prefix.isEmpty()) {
            if (!path.startsWith(prefix + '.')) {
                continue;
            }
            // Extract the next segment after the prefix
            QString remainder = path.mid(prefix.length() + 1);
            int dotIndex = remainder.indexOf('.');
            QString nextSegment = dotIndex != -1 ? remainder.left(dotIndex) : remainder;
            
            // Only add unique segments
            if (!seen.contains(nextSegment)) {
                seen.insert(nextSegment);
                result.append(nextSegment);
            }
        } else {
            // No prefix - get the first segment of each path
            int dotIndex = path.indexOf('.');
            QString firstSegment = dotIndex != -1 ? path.left(dotIndex) : path;
            
            // Only add unique segments
            if (!seen.contains(firstSegment)) {
                seen.insert(firstSegment);
                result.append(firstSegment);
            }
        }
    }
    
    return result;
}

bool TomlDocument::contains(const QString& key) const
{
    for (const Entry& entry : m_entries) {
        if (entry.path == key) {
            return true;
        }
    }
    return false;
}

QString TomlDocument::value(const QString& key) const
{
    QString result;
    bool found = false;
    for (const Entry& entry : m_entries) {
        if (entry.path == key) {
            result = unquote(entry.rawValue);
            found = true;
        }
    }
    return found ? result : QString();
}

QStringList TomlDocument::stringList(const QString& key) const
{
    QStringList result;
    
    for (const Entry& entry : m_entries) {
        if (entry.path == key) {
            QString value = entry.rawValue.trimmed();
            
            // Handle array format: [ "value1", "value2", ... ]
            if (value.startsWith('[') && value.endsWith(']')) {
                // Remove the outer brackets
                QString arrayContent = value.mid(1, value.length() - 2);
                
                // Parse array elements, handling comments, trailing commas, and multi-line strings
                QStringList items;
                QString currentItem;
                bool inQuotes = false;
                QChar quoteChar;
                bool inMultiLineString = false;
                int i = 0;
                
                while (i < arrayContent.length()) {
                    QChar c = arrayContent[i];
                    
                    // Handle quote characters
                    if (!inMultiLineString && c == '\"' && (i == 0 || arrayContent[i-1] != '\\')) {
                        if (!inQuotes) {
                            inQuotes = true;
                            quoteChar = c;
                        } else if (quoteChar == c) {
                            inQuotes = false;
                        }
                    }
                    // Handle multi-line string start
                    else if (!inQuotes && c == '\"' && i + 2 < arrayContent.length() && 
                             arrayContent[i+1] == '\"' && arrayContent[i+2] == '\"' && 
                             (i == 0 || arrayContent[i-1] != '\\')) {
                        inMultiLineString = true;
                        i += 2; // Skip the next two quotes
                    }
                    // Handle multi-line string end
                    else if (!inQuotes && inMultiLineString && c == '\"' && 
                             i + 2 < arrayContent.length() && 
                             arrayContent[i+1] == '\"' && arrayContent[i+2] == '\"') {
                        inMultiLineString = false;
                        i += 2; // Skip the next two quotes
                    }
                    // Handle comma - end of current item
                    else if (!inQuotes && !inMultiLineString && c == ',') {
                        // Trim and add item if not empty
                        QString trimmedItem = currentItem.trimmed();
                        if (!trimmedItem.isEmpty()) {
                            // Remove surrounding quotes if present
                            if ((trimmedItem.startsWith('"') && trimmedItem.endsWith('"')) ||
                                (trimmedItem.startsWith('\'') && trimmedItem.endsWith('\''))) {
                                trimmedItem = trimmedItem.mid(1, trimmedItem.length() - 2);
                            }
                            items.append(trimmedItem);
                        }
                        currentItem.clear();
                    }
                    // Handle comment - skip to end of line
                    else if (!inQuotes && !inMultiLineString && c == '#') {
                        // Skip to end of line or end of content
                        while (i < arrayContent.length() && arrayContent[i] != '\n') {
                            i++;
                        }
                    }
                    // Handle newlines
                    else if (c == '\n') {
                        // Newlines are part of multi-line strings, otherwise they're whitespace
                        if (inMultiLineString) {
                            currentItem += c;
                        }
                    }
                    // Regular character
                    else {
                        currentItem += c;
                    }
                    i++;
                }
                
                // Add the last item if it exists
                QString trimmedItem = currentItem.trimmed();
                if (!trimmedItem.isEmpty()) {
                    // Remove surrounding quotes if present
                    if ((trimmedItem.startsWith('"') && trimmedItem.endsWith('"')) ||
                        (trimmedItem.startsWith('\'') && trimmedItem.endsWith('\''))) {
                        trimmedItem = trimmedItem.mid(1, trimmedItem.length() - 2);
                    }
                    items.append(trimmedItem);
                }
                
                result = items;
            } else {
                // Not an array, return the single value as a list
                result.append(unquote(value));
            }
            break;
        }
    }
    
    return result;
}

int TomlDocument::tableArrayCount(const QString& key) const
{
    // Return the count of array-of-table entries for the given key
    // If the key doesn't exist in m_arrayTables, return 0
    return m_arrayTables.value(key, 0);
}

QString TomlDocument::unquote(const QString& raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.length() >= 2 && trimmed.at(0) == trimmed.at(trimmed.length() - 1)
        && (trimmed.at(0) == QLatin1Char('"') || trimmed.at(0) == QLatin1Char('\''))) {
        return trimmed.mid(1, trimmed.length() - 2);
    }
    return trimmed;
}
