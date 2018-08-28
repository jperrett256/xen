#ifndef MD5_H
#define MD5_H

#include <stdint.h>
#include <stddef.h>

/**
 * md5_sum - MD5 hash for a data block
 * @addr: Pointers to the data area
 * @len: Lengths of the data block
 * @mac: Buffer for the hash
 */
void md5_sum(const uint8_t *addr, const size_t len, uint8_t *mac);


struct MD5Context {
        uint32_t buf[4];
        uint32_t bits[2];
        uint8_t in[64];
};
typedef struct MD5Context MD5_CTX;
void MD5Init(struct MD5Context *context);
void MD5Update(struct MD5Context *context, unsigned char const *buf, unsigned len);
void MD5Final(unsigned char digest[16], struct MD5Context *context);

#endif
