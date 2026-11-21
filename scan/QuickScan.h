#pragma once
#include "ScanEngine.h"

// 兼容层：保留旧类名，内部直接委托给 VolumeSectorScanner。
class QuickScan : public ScanEngine
{
public:
    // 由 GUI 传入的外部取消标志（例如 MainWnd::m_cancel）；
    // 如果不设置，则退回使用 ScanEngine 自己的 m_cancelled。
    void SetExternalCancelFlag(std::atomic<bool>* flag) { m_externalCancel = flag; }

    ScanResult Run(SectorReader& reader, const Partition& partition) override;

private:
    std::atomic<bool>* m_externalCancel = nullptr;
};
