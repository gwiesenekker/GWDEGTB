#ifndef GWDEGTB_CRC32C_H
#define GWDEGTB_CRC32C_H

/* Internal checksum implementation. Both paths use reflected Castagnoli,
 * initial state ~0 and final complement; existing disk checksums are unchanged. */
#include "compat.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static compat_once_t crc32c_once = COMPAT_ONCE_INITIALIZER;
static uint32_t crc32c_table[256];

static void initialize_crc32c_table(void)
{
    for (unsigned entry = 0; entry < 256; ++entry) {
        uint32_t value = entry;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^
                    (UINT32_C(0x82f63b78) & (UINT32_C(0) - (value & 1)));
        crc32c_table[entry] = value;
    }
}

static uint32_t crc32c_portable(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    compat_once(&crc32c_once, initialize_crc32c_table);
    for (size_t i = 0; i < size; ++i)
        crc = crc32c_table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define EGTB_CRC32C_X86 1
__attribute__((target("sse4.2")))
static uint32_t crc32c_hardware(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint64_t crc = UINT32_MAX;
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        /* Unaligned buffers are supported without aliasing violations. */
        memcpy(&word, bytes, sizeof(word));
        crc = _mm_crc32_u64(crc, word);
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    uint32_t tail = (uint32_t)crc;
    while (size-- != 0)
        tail = _mm_crc32_u8(tail, *bytes++);
    return ~tail;
}
#endif

static uint32_t crc32c(const void *data, size_t size)
{
#if defined(EGTB_CRC32C_X86) && !defined(EGTB_CRC32C_FORCE_PORTABLE)
    /* Runtime guard also makes generic x86 builds safe on older CPUs. */
    if (compat_cpu_has_sse42())
        return crc32c_hardware(data, size);
#endif
    return crc32c_portable(data, size);
}
#endif
