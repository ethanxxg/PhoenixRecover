#include "FatStructs.h"

#include <ctime>

namespace {

uint64_t SystemTimeToFiletime100ns(int year, int month, int day, int hour, int minute, int second,
                                   int millis)
{
    struct tm t {};
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = minute;
    t.tm_sec   = second;
#if defined(_WIN32)
    const time_t secs = _mkgmtime(&t);
#else
    const time_t secs = timegm(&t);
#endif
    if (secs == static_cast<time_t>(-1))
        return 0;
    constexpr uint64_t kEpochDiff = 11644473600ULL;
    uint64_t ft = (static_cast<uint64_t>(secs) + kEpochDiff) * 10000000ULL;
    ft += static_cast<uint64_t>(millis) * 10000ULL;
    return ft;
}

} // namespace

uint64_t FatDateTimeToFiletime(uint16_t date, uint16_t time, uint8_t timeTenth)
{
    if (date == 0)
        return 0;

    const int year   = 1980 + ((date >> 9) & 0x7F);
    const int month  = (date >> 5) & 0x0F;
    const int day    = date & 0x1F;
    const int hour   = (time >> 11) & 0x1F;
    const int minute = (time >> 5) & 0x3F;
    int second       = (time & 0x1F) * 2;
    int millis       = (timeTenth >= 100) ? (timeTenth - 100) * 10 : 0;
    if (timeTenth < 100)
        second += 1;

    return SystemTimeToFiletime100ns(year, month, day, hour, minute, second, millis);
}
