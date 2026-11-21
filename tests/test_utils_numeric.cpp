// 轻量单元测试：不链接 Qt / Windows I/O，仅验证 Utils 数值与解析工具。
#include <cstdint>
#include <cstdlib>

#include "common/Utils.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) return 1; \
    } while (0)

int main()
{
    uint8_t b[8]{};

    b[0] = 0x34;
    b[1] = 0x12;
    CHECK(Utils::ReadLE16(b) == 0x1234);

    b[0] = 0x78;
    b[1] = 0x56;
    b[2] = 0x34;
    b[3] = 0x12;
    CHECK(Utils::ReadLE32(b) == 0x12345678);

    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<uint8_t>(i);
    CHECK(Utils::ReadLE64(b) == 0x0706050403020100ULL);

    CHECK(Utils::AlignUp(0, 512) == 0);
    CHECK(Utils::AlignUp(1, 4) == 4);
    CHECK(Utils::AlignUp(8, 8) == 8);

    b[0] = 0xFF;
    CHECK(Utils::ReadSignedLE(b, 1) == -1);

    b[0] = 0x02;
    b[1] = 0xFC;
    CHECK(Utils::ReadSignedLE(b, 2) == -1022);

    b[0] = 0x00;
    b[1] = 0x00;
    b[2] = 0x00;
    CHECK(Utils::ReadSignedLE(b, 3) == 0);

    return 0;
}
