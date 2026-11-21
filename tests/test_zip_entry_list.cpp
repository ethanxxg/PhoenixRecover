#include "repair/ZipEntryList.h"

#include <QFile>
#include <QTemporaryFile>

#include <cassert>

namespace {

void appendU16(QByteArray &out, quint16 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

void appendStoredEntry(QByteArray &zip, const QByteArray &name, const QByteArray &data)
{
    appendU32(zip, 0x04034b50u);
    appendU16(zip, 20);
    appendU16(zip, 0);
    appendU16(zip, 0);
    appendU16(zip, 0);
    appendU16(zip, 0);
    appendU32(zip, 0);
    appendU32(zip, static_cast<quint32>(data.size()));
    appendU32(zip, static_cast<quint32>(data.size()));
    appendU16(zip, static_cast<quint16>(name.size()));
    appendU16(zip, 0);
    zip.append(name);
    zip.append(data);
}

} // namespace

int main()
{
    QTemporaryFile temp;
    assert(temp.open());
    temp.setAutoRemove(true);

    QByteArray zip;
    appendStoredEntry(zip, "[Content_Types].xml", "types");
    appendStoredEntry(zip, "word/document.xml", "document");
    assert(temp.write(zip) == zip.size());
    temp.flush();
    temp.close();

    const QStringList names = listZipLocalEntryNamesFromPath(temp.fileName());
    assert(names.size() == 2);
    assert(names.at(0) == QStringLiteral("[Content_Types].xml"));
    assert(names.at(1) == QStringLiteral("word/document.xml"));
    return 0;
}
