#pragma once

#include "ScanEngine.h"
#include "../recover/FragmentStitchPolicy.h"

#include <atomic>
#include <memory>

class FileSystem;

class VolumeSectorScanner
{
public:
    void SetCancelFlag(std::atomic<bool> *flag) { cancelFlag_ = flag; }
    void SetProgressCallback(ProgressCallback cb) { onProgress_ = std::move(cb); }
    void SetFileFoundCallback(FileFoundCallback cb) { onFileFound_ = std::move(cb); }
    void SetFragmentStitchPolicy(const FragmentStitchPolicy& policy) { fragmentStitchPolicy_ = policy; }

    ScanResult Run(SectorReader &reader, const Partition &partition);

private:
    std::atomic<bool> *cancelFlag_ = nullptr;
    ProgressCallback onProgress_;
    FileFoundCallback onFileFound_;
    FragmentStitchPolicy fragmentStitchPolicy_;
};
