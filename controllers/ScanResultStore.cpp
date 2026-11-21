#include "ScanResultStore.h"

#include "../log/Logger.h"
#include <QtCore/QWriteLocker>
#include <QtCore/QReadLocker>

namespace {
QString otherFixedSubtype(const QString &ext)
{
    const QString e = ext.trimmed().toLower();
    static const QStringList allowed = QStringList()
        << QStringLiteral("sys")
        << QStringLiteral("exe")
        << QStringLiteral("dll")
        << QStringLiteral("txt")
        << QStringLiteral("log")
        << QStringLiteral("ini")
        << QStringLiteral("cfg")
        << QStringLiteral("dat")
        << QStringLiteral("tmp")
        << QStringLiteral("bak");
    return allowed.contains(e) ? e : QStringLiteral("其他");
}

QString relativePathForStore(const ScanFileDto &dto)
{
    QString rel = dto.relativePath.trimmed();
    if (rel.isEmpty()) {
        rel = dto.fullPath;
    }
    const int colon = rel.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        rel = rel.mid(colon + 1);
    }
    if (rel.startsWith(QLatin1Char('\\')) || rel.startsWith(QLatin1Char('/'))) {
        rel = rel.mid(1);
    }
    rel.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (rel.endsWith(QLatin1Char('\\'))) {
        rel.chop(1);
    }
    return rel;
}

bool hasUnresolvedPathSegment(const QString &relPath)
{
    const int lt = relPath.indexOf(QLatin1Char('<'));
    if (lt < 0) {
        return false;
    }
    const int question = relPath.indexOf(QLatin1Char('?'), lt + 1);
    const int gt = relPath.indexOf(QLatin1Char('>'), lt + 1);
    return question >= 0 && gt > question;
}
}

void ScanResultStore::clear()
{
    QWriteLocker locker(&lock_);
    ++revision_;
    files_.clear();
    insertOrder_.clear();
    pathChildDirectories_.clear();
    pathNodeCounts_.clear();
    pathDirectChildIds_.clear();
    typeCategoryChildren_.clear();
    typeNodeCounts_.clear();
    typeNodeFileIds_.clear();
    typeCategories_.clear();
    hasScanVolume_ = false;
    previewResidentBlobs_.clear();
    previewFileNodes_.clear();
    recoveredFileNodes_.clear();
}

bool ScanResultStore::upsert(const ScanFileDto &dto)
{
    QWriteLocker locker(&lock_);
    if (files_.contains(dto.id)) {
        const ScanFileDto old = files_.value(dto.id);
        if (old.fullPath.compare(dto.fullPath, Qt::CaseInsensitive) == 0 &&
            old.extension.compare(dto.extension, Qt::CaseInsensitive) == 0 &&
            old.relativePath.compare(dto.relativePath, Qt::CaseInsensitive) == 0 &&
            old.isDirectory == dto.isDirectory &&
            old.previewKind == dto.previewKind &&
            old.previewVolumeOffset == dto.previewVolumeOffset &&
            old.previewReadLength == dto.previewReadLength) {
            return false;
        }
        files_[dto.id] = dto;
        syncPreviewSupplementsLocked(dto);
        rebuildIndexesLocked();
        ++revision_;
        if ((revision_ % 512) == 0) {
            LOG_INFO(L"ScanResultStore::upsert rebuild"
                     L" revision=" + std::to_wstring(static_cast<uint64_t>(revision_)) +
                     L" files=" + std::to_wstring(static_cast<uint64_t>(files_.size())) +
                     L" rootDirs=" + std::to_wstring(static_cast<uint64_t>(pathChildDirectories_.value(QString()).size())) +
                     L" rootItems=" + std::to_wstring(static_cast<uint64_t>(pathDirectChildIds_.value(QString()).size())));
        }
        return true;
    }

    files_.insert(dto.id, dto);
    insertOrder_.append(dto.id);
    syncPreviewSupplementsLocked(dto);
    indexPathLocked(dto);
    indexTypeLocked(dto);
    ++revision_;
    if ((revision_ % 512) == 0) {
        LOG_INFO(L"ScanResultStore::upsert append"
                 L" revision=" + std::to_wstring(static_cast<uint64_t>(revision_)) +
                 L" files=" + std::to_wstring(static_cast<uint64_t>(files_.size())) +
                 L" rootDirs=" + std::to_wstring(static_cast<uint64_t>(pathChildDirectories_.value(QString()).size())) +
                 L" rootItems=" + std::to_wstring(static_cast<uint64_t>(pathDirectChildIds_.value(QString()).size())));
    }
    return true;
}

void ScanResultStore::rebuildIndexesLocked()
{
    pathChildDirectories_.clear();
    pathNodeCounts_.clear();
    pathDirectChildIds_.clear();
    typeCategoryChildren_.clear();
    typeNodeCounts_.clear();
    typeNodeFileIds_.clear();
    typeCategories_.clear();

    for (auto it = files_.cbegin(); it != files_.cend(); ++it) {
        indexPathLocked(it.value());
        indexTypeLocked(it.value());
    }
}

quint64 ScanResultStore::revision() const
{
    QReadLocker locker(&lock_);
    return revision_;
}

int ScanResultStore::fileCount() const
{
    QReadLocker locker(&lock_);
    return files_.size();
}

bool ScanResultStore::tryGetFile(quint64 id, ScanFileDto *out) const
{
    QReadLocker locker(&lock_);
    if (!files_.contains(id)) {
        return false;
    }
    if (out) {
        *out = files_.value(id);
    }
    return true;
}

QVector<ScanFileDto> ScanResultStore::filesByInsertOrder(int offset, int count) const
{
    QReadLocker locker(&lock_);
    QVector<ScanFileDto> result;
    const int end = qMin(offset + count, insertOrder_.size());
    if (offset >= insertOrder_.size() || offset < 0) {
        return result;
    }
    result.reserve(end - offset);
    for (int i = offset; i < end; ++i) {
        const quint64 id = insertOrder_.at(i);
        auto it = files_.constFind(id);
        if (it != files_.cend()) {
            result.append(it.value());
        }
    }
    return result;
}

QVector<QString> ScanResultStore::typeCategories() const
{
    QReadLocker locker(&lock_);
    QVector<QString> out;
    out.reserve(typeCategories_.size());
    for (auto it = typeCategories_.cbegin(); it != typeCategories_.cend(); ++it) {
        out.append(*it);
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        static const QStringList preferredOrder = QStringList()
            << QStringLiteral("图片")
            << QStringLiteral("视频")
            << QStringLiteral("文档")
            << QStringLiteral("音频")
            << QStringLiteral("压缩文件")
            << QStringLiteral("邮件")
            << QStringLiteral("书签")
            << QStringLiteral("其他")
            << QStringLiteral("未保存的文件");
        const int aIndex = preferredOrder.indexOf(a);
        const int bIndex = preferredOrder.indexOf(b);
        if (aIndex >= 0 || bIndex >= 0) {
            if (aIndex < 0) {
                return false;
            }
            if (bIndex < 0) {
                return true;
            }
            if (aIndex != bIndex) {
                return aIndex < bIndex;
            }
        }
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QStringList ScanResultStore::pathChildDirectories(const QString &parentRel) const
{
    QReadLocker locker(&lock_);
    return pathChildDirectories_.value(parentRel);
}

int ScanResultStore::pathNodeCount(const QString &relPath) const
{
    QReadLocker locker(&lock_);
    return pathNodeCounts_.value(relPath);
}

qint64 ScanResultStore::pathDirectFileBytes(const QString &parentRel) const
{
    QReadLocker locker(&lock_);
    qint64 total = 0;
    const QVector<quint64> ids = pathDirectChildIds_.value(parentRel);
    for (int i = 0; i < ids.size(); ++i) {
        const quint64 id = ids.at(i);
        if (!files_.contains(id)) {
            continue;
        }
        const ScanFileDto &dto = files_[id];
        if (dto.isDirectory) {
            continue;
        }
        total += dto.sizeBytes;
    }
    return total;
}

QVector<quint64> ScanResultStore::pathDirectChildIds(const QString &parentRel) const
{
    QReadLocker locker(&lock_);
    return pathDirectChildIds_.value(parentRel);
}

QStringList ScanResultStore::typeCategoryChildren(const QString &category) const
{
    QReadLocker locker(&lock_);
    return typeCategoryChildren_.value(category);
}

int ScanResultStore::typeNodeCount(const QString &key) const
{
    QReadLocker locker(&lock_);
    return typeNodeCounts_.value(key);
}

QVector<quint64> ScanResultStore::typeNodeFileIds(const QString &key) const
{
    QReadLocker locker(&lock_);
    return typeNodeFileIds_.value(key);
}

void ScanResultStore::indexPathLocked(const ScanFileDto &dto)
{
    const QString rel = relativePathForStore(dto);
    if (rel.isEmpty()) {
        return;
    }

    // NTFS may emit temporary fallback segments like "<195134?>" before
    // parent directories are fully resolved. Do not materialize those
    // placeholders into the path tree; wait for the later corrected callback.
    if (hasUnresolvedPathSegment(rel)) {
        if ((revision_ % 2048) == 0) {
            LOG_INFO(L"ScanResultStore::indexPathLocked skip unresolved path"
                     L" rel=[" + rel.toStdWString() + L"]"
                     L" id=" + std::to_wstring(static_cast<uint64_t>(dto.id)));
        }
        return;
    }

    QString parentRel;
    QString nodeRel = rel;
    if (dto.isDirectory) {
        const int slash = rel.lastIndexOf(QLatin1Char('\\'));
        parentRel = (slash >= 0) ? rel.left(slash) : QString();
    } else {
        const int slash = rel.lastIndexOf(QLatin1Char('\\'));
        parentRel = (slash >= 0) ? rel.left(slash) : QString();
        nodeRel = parentRel;
    }

    pathNodeCounts_[QString()] += 1;
    pathDirectChildIds_[parentRel].append(dto.id);

    if (nodeRel.isEmpty()) {
        return;
    }

    const QStringList folderParts = nodeRel.split(QChar('\\'), QString::SkipEmptyParts);
    QString currentParentRel;
    for (int i = 0; i < folderParts.size(); ++i) {
        const QString seg = folderParts.at(i);
        const QString currentRel = currentParentRel.isEmpty() ? seg : (currentParentRel + QStringLiteral("\\") + seg);
        pathNodeCounts_[currentRel] += 1;
        if (!pathChildDirectories_[currentParentRel].contains(seg, Qt::CaseInsensitive)) {
            pathChildDirectories_[currentParentRel].append(seg);
        }
        currentParentRel = currentRel;
    }
}

void ScanResultStore::indexTypeLocked(const ScanFileDto &dto)
{
    if (dto.isDirectory) {
        return;
    }

    QString ext = dto.extension.trimmed().toLower();
    const QString pics = QStringLiteral(
        "jpg,png,jpeg,gif,tif,eps,psd,bmp,ico,3mf,ai,ani,apm,crw,cur,dds,djvu,dsc,emf,exr,gmp,gpd,gpr,hdr,heic,heif,hp,icns,icon,jp2,jpf,jxr,mnw,pbm,pgm,pm,ppm,ps,raw,sci,scp,scr,sgi,shp,stl,svg,tga,tiff,tpl,webp,wmf,xar,xbm,xcf,xpm");
    const QString vids = QStringLiteral(
        "mp4,mov,avi,swf,m4v,wmv,3g2,3gp,3gpp,3gpp2,aaf,asf,bin,dat,dir,dv,fbx,flv,h264,m2t,m2ts,m3u,mkv,mod,mpeg,mts,mxf,ogv,rms,rmvb,rpm,rt,scm,ts,webm");
    const QString aud = QStringLiteral(
        "mp3,wav,wma,m4a,ape,flac,aac,acc,acm,aifc,aiff,al,amr,au,dls,dsp,emx,flp,ins,jam,lib,mid,midi,mpc,mpga,"
        "mui,pga,ogg,qcp,rbs,sb,sct,smp,sng,tsp,val,voc");
    const QString docs = QStringLiteral(
        "doc,docx,pdf,xls,xlsx,ppt,pptx,3ds,accdb,addin,ascii,bak,cfg,chk,chm,config,cpp,crwl,cs,css,csv,db,dot,"
        "dotm,dotx,dox,dwg,enc,eot,epub,et,fcpevent,fdf,fft,fil,fs,gpx,gthr,htm,html,ics,inf,ipynb,java,js,json,"
        "key,lit,lnk,log,lst,lzh,m,manifest,md,mdb,me,mhtml,mm,mmap,mobi,mof,msg,numbers,odg,odp,ods,odt,old,ort,"
        "ott,p7s,pem,pfx,php,plist,pot,potx,pub,py,rad,readme,rpt,rst,rtf,sam,scpt,sitx,slk,sin,srt,swift,sxc,tab,"
        "text,thmx,tlb,tm,ttf,txt,url,utf8,vbs,vcf,vcproj,woff,woff2,wpl,wps,wpt,xib,xlf,xmind,xml,xsi,yml");
    const QString arch = QStringLiteral(
        "zip,rar,7z,tar,apk,bat,bz2,cab,cat,cbt,deb,dist,gz,img,iso,jar,lzma,msi,otf,pack,pak,pf,pfb,pup,ro,rev,sdi,sfx,"
        "sh,tgz,ttc,wim,xz,n,zap,zfsendtotarget,z");
    const QString mails = QStringLiteral("edb,eml,idx,pst");
    const QString marks = QStringLiteral("html,url,webloc");
    const QString unsaved = QStringLiteral("asd,tmp,~");
    QString category = QStringLiteral("其他");
    if (pics.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("图片");
    else if (vids.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("视频");
    else if (aud.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("音频");
    else if (docs.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("文档");
    else if (arch.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("压缩文件");
    else if (mails.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("邮件");
    else if (marks.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("书签");
    else if (unsaved.split(QLatin1Char(',')).contains(ext)) category = QStringLiteral("未保存的文件");

    QString subLabel = ext.isEmpty() ? QStringLiteral("其他") : ext;
    if (category == QStringLiteral("其他")) {
        subLabel = otherFixedSubtype(ext);
    }
    const QString subKey = category + QStringLiteral("::") + subLabel;
    typeCategories_.insert(category);
    typeNodeCounts_[category] += 1;
    typeNodeCounts_[subKey] += 1;
    typeNodeFileIds_[category].append(dto.id);
    typeNodeFileIds_[subKey].append(dto.id);
    if (!typeCategoryChildren_[category].contains(subLabel, Qt::CaseInsensitive)) {
        typeCategoryChildren_[category].append(subLabel);
    }
}

void ScanResultStore::syncPreviewSupplementsLocked(const ScanFileDto &dto)
{
    switch (dto.previewKind) {
    case ScanPreviewKind::None:
        previewResidentBlobs_.remove(dto.id);
        previewFileNodes_.remove(dto.id);
        break;
    case ScanPreviewKind::ResidentBlob:
        previewFileNodes_.remove(dto.id);
        break;
    case ScanPreviewKind::VolumeSlice:
        previewResidentBlobs_.remove(dto.id);
        previewFileNodes_.remove(dto.id);
        break;
    case ScanPreviewKind::FullNode:
        previewResidentBlobs_.remove(dto.id);
        break;
    }
}

void ScanResultStore::setScanVolumePartition(const Partition &partition)
{
    QWriteLocker locker(&lock_);
    scanVolume_ = partition;
    hasScanVolume_ = true;
}

bool ScanResultStore::tryGetScanVolumePartition(Partition *out) const
{
    if (!out) {
        return false;
    }
    QReadLocker locker(&lock_);
    if (!hasScanVolume_) {
        return false;
    }
    *out = scanVolume_;
    return true;
}

void ScanResultStore::setPreviewResidentBlob(quint64 id, const QByteArray &bytes)
{
    QWriteLocker locker(&lock_);
    previewResidentBlobs_.insert(id, bytes);
}

void ScanResultStore::setPreviewFileNodeSnapshot(quint64 id, const FileNode &node)
{
    QWriteLocker locker(&lock_);
    previewFileNodes_.insert(id, node);
}

bool ScanResultStore::tryGetPreviewResidentBlob(quint64 id, QByteArray *out) const
{
    if (!out) {
        return false;
    }
    QReadLocker locker(&lock_);
    auto it = previewResidentBlobs_.constFind(id);
    if (it == previewResidentBlobs_.cend()) {
        return false;
    }
    *out = it.value();
    return true;
}

bool ScanResultStore::tryGetPreviewFileNode(quint64 id, FileNode *out) const
{
    if (!out) {
        return false;
    }
    QReadLocker locker(&lock_);
    auto it = previewFileNodes_.constFind(id);
    if (it == previewFileNodes_.cend()) {
        return false;
    }
    *out = it.value();
    return true;
}

void ScanResultStore::setRecoveredFileNode(quint64 id, const FileNode &node)
{
    QWriteLocker locker(&lock_);
    recoveredFileNodes_.insert(id, node);
}

bool ScanResultStore::tryGetRecoveredFileNode(quint64 id, FileNode *out) const
{
    if (!out) {
        return false;
    }
    QReadLocker locker(&lock_);
    auto it = recoveredFileNodes_.constFind(id);
    if (it == recoveredFileNodes_.cend()) {
        return false;
    }
    *out = it.value();
    return true;
}

QHash<quint64, FileNode> ScanResultStore::allRecoveredFileNodes() const
{
    QReadLocker locker(&lock_);
    return recoveredFileNodes_;
}
