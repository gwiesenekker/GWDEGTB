#include "crc32c.h"
#include <stdio.h>

static uint32_t reference(const unsigned char *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    while (n-- != 0) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0x82f63b78) : 0);
    }
    return ~crc;
}

int main(void)
{
    unsigned char data[4096 + 8];
    uint32_t random = 1234567;
    if (crc32c(NULL, 0) != 0 ||
        crc32c("123456789", 9) != UINT32_C(0xe3069283))
        return 1;
    for (size_t i = 0; i < sizeof(data); ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        data[i] = (unsigned char)random;
    }
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t length = 0; length <= 4096; ++length) {
            uint32_t expected = reference(data + offset, length);
            if (crc32c(data + offset, length) != expected ||
                crc32c_portable(data + offset, length) != expected)
                return 1;
#ifdef EGTB_CRC32C_X86
            if (__builtin_cpu_supports("sse4.2") &&
                crc32c_hardware(data + offset, length) != expected)
                return 1;
#endif
        }
    }
    puts("CRC32C: known vectors, all lengths 0..4096 and offsets 0..7 passed");
    return 0;
}
