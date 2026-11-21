#include "QuickScan.h"

#include "VolumeSectorScanner.h"

ScanResult QuickScan::Run(SectorReader& reader, const Partition& partition)
{
    VolumeSectorScanner scanner;
    scanner.SetCancelFlag(m_externalCancel ? m_externalCancel : &m_cancelled);
    scanner.SetProgressCallback(m_onProgress);
    scanner.SetFileFoundCallback(m_onFileFound);
    return scanner.Run(reader, partition);
}
