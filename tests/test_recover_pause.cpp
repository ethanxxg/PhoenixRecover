#include "recover/RecoverEngine.h"

#include <cassert>
#include <vector>

int main()
{
    RecoverEngine engine;
    bool pauseCallbackCalled = false;
    engine.SetPauseCallback([&pauseCallbackCalled]() {
        pauseCallbackCalled = true;
        return false;
    });

    FileNode node;
    node.id = 1;
    node.name = L"test-only.bin";
    node.size = 1;

    SectorReader reader;
    Partition partition;
    const RecoverBatchResult result = engine.RecoverBatch(
        std::vector<FileNode>{node}, L"", false, reader, partition, nullptr);

    assert(pauseCallbackCalled);
    assert(result.cancelled);
    assert(result.details.empty());
    return 0;
}
