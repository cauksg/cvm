#ifndef __SBI_CVM_MULTI_KEY_H__
#define __SBI_CVM_MULTI_KEY_H__

#include <sbi/sbi_types.h>

/* Clear the k-th bit. */
#define	CLEAR_K_BIT(num, k)	(num &= ~(1UL << k))
/* Set the k-th bit. */
#define SET_K_BIT(num, k) (num |= (1UL << k))
#define GET_K_BIT(num, k) (num >> k & 1UL)
/* READ_CSR/WRITE_CSR for test */
#define READ_CSR(name) key_regs->name
#define WRITE_CSR(name, reg) key_regs->name = reg

#define LOOP_MAX 1024
#define KEY_ID_LEN 5
#define KEY_ID_NUM (1UL << KEY_ID_LEN)

struct multi_key_regs {
    /* 0x0: enc config CSR */
    /*
        mkeycfg[4:0]:   key_id
        mkeycfg[6:5]:   mode
        mkeycfg[7]:     tweak_flag
        mkeycfg[8]:     memenc_enable
        mkeycfg[32]:    random_ready_flag
        mkeycfg[33]:    key_expansion_idle
        mkeycfg[34]:    last_req_accepted
        mkeycfg[35]:    cfg_succesd
        mkeycfg[63]:    key_init_req
    */
    uint64_t mkeycfg;
    /* 0x8, 0x10: 2 64-bit key registers, write-only, configured by sw.*/
    uint64_t mswkey1, mswkey2;
    /* 0x18: Max width of Paddr, read-only, 0xF_FFFF_FFFF */
    uint64_t mmwpaddr;
    /* 0x20: Mask of Paddr for KeyId, read-only, 0x1F0_0000_0000 */
    uint64_t mkeymsk;
    /* 0x28: Version, read-only, [63: 32] = 0x00010001 */
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
int config_tweak_key_with_rng();
int config_tweak_key_with_sw();

int config_key_id_with_rng(uint64_t keyid);
int config_key_id_with_sw(uint64_t keyid);

uint64_t gen_key_id();
void release_key_id(uint64_t keyid);

int test_cfg();

#endif