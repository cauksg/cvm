# 0 "sbi_multi_key.c"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/riscv64-linux-gnu/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "sbi_multi_key.c"
# 1 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/mkey/multi_key.h" 1



# 1 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/sbi/sbi_types.h" 1
# 17 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/sbi/sbi_types.h"
typedef char s8;
typedef unsigned char u8;
typedef unsigned char uint8_t;

typedef short s16;
typedef unsigned short u16;
typedef short int16_t;
typedef unsigned short uint16_t;

typedef int s32;
typedef unsigned int u32;
typedef int int32_t;
typedef unsigned int uint32_t;


typedef long s64;
typedef unsigned long u64;
typedef long int64_t;
typedef unsigned long uint64_t;
# 47 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/sbi/sbi_types.h"
typedef int bool;
typedef unsigned long ulong;
typedef unsigned long uintptr_t;
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long virtual_addr_t;
typedef unsigned long virtual_size_t;
typedef unsigned long physical_addr_t;
typedef unsigned long physical_size_t;

typedef uint16_t le16_t;
typedef uint16_t be16_t;
typedef uint32_t le32_t;
typedef uint32_t be32_t;
typedef uint64_t le64_t;
typedef uint64_t be64_t;
# 5 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/mkey/multi_key.h" 2
# 15 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/mkey/multi_key.h"
struct multi_key_regs {
# 28 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/mkey/multi_key.h"
    uint64_t mkeycfg;

    uint64_t mswkey1, mswkey2;

    uint64_t mmwpaddr;

    uint64_t mkeymsk;

    uint64_t mkeyver;
};

uint64_t read_key_id(uint64_t *reg64);
uint64_t read_mode(uint64_t *reg64);
uint64_t read_tweak_flag(uint64_t *reg64);
uint64_t read_random_ready_flag(uint64_t *reg64);
uint64_t read_key_expansion_idle(uint64_t *reg64);
uint64_t read_last_req_accepted(uint64_t *reg64);
uint64_t read_cfg_succesd(uint64_t *reg64);
void write_key_id(uint64_t *reg64, uint64_t val);
void write_mode(uint64_t *reg64, uint64_t val);
void write_tweak_flag(uint64_t *reg64, uint64_t val);
void write_memenc_enable(uint64_t *reg64, uint64_t val);
void write_key_init_req(uint64_t *reg64, uint64_t val);

uint64_t multi_key_sys_init(bool memenc_enable);
int config_tweak_key();
int config_key_id();

int test_cfg();
# 2 "sbi_multi_key.c" 2
# 1 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/sbi/sbi_string.h" 1
# 20 "/nfs/home/wangshiwen/riscv-kvm/opensbi/include/sbi/sbi_string.h"
int sbi_strcmp(const char *a, const char *b);

int sbi_strncmp(const char *a, const char *b, size_t count);

size_t sbi_strlen(const char *str);

size_t sbi_strnlen(const char *str, size_t count);

char *sbi_strcpy(char *dest, const char *src);

char *sbi_strncpy(char *dest, const char *src, size_t count);

char *sbi_strchr(const char *s, int c);

char *sbi_strrchr(const char *s, int c);

void *sbi_memset(void *s, int c, size_t count);

void *sbi_memcpy(void *dest, const void *src, size_t count);

void *sbi_memmove(void *dest, const void *src, size_t count);

int sbi_memcmp(const void *s1, const void *s2, size_t count);

void *sbi_memchr(const void *s, int c, size_t count);
# 3 "sbi_multi_key.c" 2

static inline uint64_t read_pos_k_bits(uint64_t *reg64, int pos, int k)
{
    uint64_t val = *reg64;
    uint64_t res = 0UL;
    for(int i = pos; i > pos - k; -- i)
    {
        res <<= 1;
        res |= (val >> i & 1UL);
    }
    return res;
}
inline uint64_t read_key_id(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 4, 5);
}
inline uint64_t read_mode(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 6, 2);
}
inline uint64_t read_tweak_flag(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 7, 1);
}
inline uint64_t read_random_ready_flag(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 32, 1);
}
inline uint64_t read_key_expansion_idle(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 33, 1);
}
inline uint64_t read_last_req_accepted(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 34, 1);
}
inline uint64_t read_cfg_succesd(uint64_t *reg64)
{
    return read_pos_k_bits(reg64, 35, 1);
}

static inline void write_pos_k_bits(uint64_t *reg64, int pos, int k, uint64_t val)
{
    uint64_t res = *reg64;
    for(int i = pos; i > pos - k; -- i) (res &= ~(1UL << i));
    uint64_t mask = 0UL;
    for(int i = 0; i < k; ++ i) (mask |= (1UL << i));
    val &= mask;
    res |= (val << (pos - k + 1));
    *reg64 = res;
}
inline void write_key_id(uint64_t *reg64, uint64_t val)
{
    write_pos_k_bits(reg64, 4, 5, val);
}
inline void write_mode(uint64_t *reg64, uint64_t val)
{
    write_pos_k_bits(reg64, 6, 2, val);
}
inline void write_tweak_flag(uint64_t *reg64, uint64_t val)
{
    write_pos_k_bits(reg64, 7, 1, val);
}
inline void write_memenc_enable(uint64_t *reg64, uint64_t val)
{
    write_pos_k_bits(reg64, 8, 1, val);
}
inline void write_key_init_req(uint64_t *reg64, uint64_t val)
{
    write_pos_k_bits(reg64, 63, 1, val);
}


struct multi_key_regs *key_regs;


uint64_t multi_key_sys_init(bool memenc_enable)
{
    if(!memenc_enable) return -1UL;


    if(!key_regs)
    {
        sbi_printf("[IIE CVM Monitor@%s] key_regs allocating... \r\n", __func__);
        key_regs = (struct multi_key_regs *)sbi_malloc(sizeof(struct multi_key_regs));
    }

    write_pos_k_bits(&key_regs->mkeyver, 63, 32, 0x00010001UL);

    write_pos_k_bits(&key_regs->mkeyver, 2, 1, 0);

    write_pos_k_bits(&key_regs->mkeyver, 1, 1, 1);

    write_pos_k_bits(&key_regs->mkeyver, 0, 1, 0);


    write_memenc_enable(&key_regs->mkeycfg, memenc_enable);


    return key_regs->mkeyver;
}

int config_tweak_key()
{

    uint64_t reg64 = key_regs->mkeycfg;
    if(!read_key_expansion_idle(&reg64) || !read_random_ready_flag(&reg64))
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);

        write_pos_k_bits(&key_regs->mkeyver, 32, 1, 0b1);

        write_pos_k_bits(&key_regs->mkeyver, 33, 1, 0b1);
        reg64 = key_regs->mkeycfg;
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, key_regs->mkeyver);
        return 0;
    }
    while(!read_key_expansion_idle(&reg64) || !read_random_ready_flag(&reg64))
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = key_regs->mkeycfg;
    }
    if(!read_last_req_accepted(&reg64))
    {
        write_tweak_flag(&reg64, 1);
        write_key_init_req(&reg64, 1);
        key_regs->mkeycfg = reg64;
        reg64 = key_regs->mkeycfg;
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        write_pos_k_bits(&key_regs->mkeyver, 34, 1, 0b1);
    }
    do {
        write_tweak_flag(&reg64, 1);
        write_key_init_req(&reg64, 1);
        key_regs->mkeycfg = reg64;
        reg64 = key_regs->mkeycfg;
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
    }while(!read_last_req_accepted(&reg64));

    if(!read_cfg_succesd(&reg64))
    {
        write_pos_k_bits(&key_regs->mkeyver, 35, 1, 0b1);
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = key_regs->mkeycfg;
    }
    while(!read_cfg_succesd(&reg64))
    {
        reg64 = key_regs->mkeycfg;
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
    }

    sbi_printf("[IIE CVM Monitor@%s] Configure tweak key successfully. \r\n", __func__);

    return 0;
}

int test_cfg()
{
# 177 "sbi_multi_key.c"
    uint64_t reg64 = key_regs->mkeycfg;
    sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
    write_pos_k_bits(&key_regs->mkeyver, 32, 1, 0b1);

    write_pos_k_bits(&key_regs->mkeyver, 33, 1, 0b1);
    reg64 = key_regs->mkeycfg;
    sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);


    return 0;
}
