#include <mkey/multi_key.h>
#include <sbi/sbi_string.h>

bool key_id_used[KEY_ID_NUM];
static uint64_t last_key_id = 0;

static inline uint64_t read_pos_k_bits(uint64_t *reg64, int pos, int k)
{
    uint64_t val = *reg64;
    uint64_t res = 0UL;
    for(int i = pos; i > pos - k; -- i) 
    {
        res <<= 1;
        res |= GET_K_BIT(val, i);
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
    for(int i = pos; i > pos - k; -- i) CLEAR_K_BIT(res, i);
    uint64_t mask = 0UL;
    for(int i = 0; i < k; ++ i) SET_K_BIT(mask, i);
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

/* temp test variable,  */
struct multi_key_regs *key_regs;

/* return mkeyver reg, write memenc_enable value into mkeycfg reg. */
uint64_t multi_key_sys_init(bool memenc_enable)
{
    if(!memenc_enable) return -1UL;

    /* temp malloc */
    if(!key_regs) 
    {
        sbi_printf("[IIE CVM Monitor@%s] key_regs allocating... \r\n", __func__);
        key_regs = (struct multi_key_regs *)sbi_malloc(sizeof(struct multi_key_regs));
    }
    sbi_memset(key_id_used, false, sizeof(key_id_used));

    uint64_t reg64 = 0;
    /* version number */
    write_pos_k_bits(&reg64, 63, 32, 0x00010001UL);
    /* single key */
    write_pos_k_bits(&reg64, 2, 1, 0);
    /* multi keys */
    write_pos_k_bits(&reg64, 1, 1, 1);
    /* integrity */
    write_pos_k_bits(&reg64, 0, 1, 0);

    /* Write memenc_enable  */
    write_memenc_enable(&reg64, memenc_enable);
    WRITE_CSR(mkeyver, reg64);

    /* Return version number */
    return key_regs->mkeyver;
}

int config_tweak_key_with_rng()
{
    uint64_t reg64 = READ_CSR(mkeycfg);
    for(int i = 0; (!read_key_expansion_idle(&reg64) || !read_random_ready_flag(&reg64))  && i < LOOP_MAX; ++ i)
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_last_req_accepted(&reg64) && i < LOOP_MAX; ++ i) 
    {
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        write_mode(&reg64, 0b10);
        write_tweak_flag(&reg64, 0b1);
        write_key_init_req(&reg64, 0b1);
        WRITE_CSR(mkeycfg, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] last_req_accepted set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    for(int i = 0; !read_cfg_succesd(&reg64) && i < LOOP_MAX; ++ i) 
    {
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] cfg_succesd set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    sbi_printf("[IIE CVM Monitor@%s] Configure tweak key successfully. \r\n", __func__);

    return 0;
}

int config_tweak_key_with_sw()
{
    uint64_t reg64 = READ_CSR(mkeycfg);
    for(int i = 0; !read_key_expansion_idle(&reg64) && i < LOOP_MAX; ++ i) 
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_last_req_accepted(&reg64) && i < LOOP_MAX; ++ i)
    {
        write_mode(&reg64, 0b01);
        write_tweak_flag(&reg64, 0b1);
        write_key_init_req(&reg64, 0b1);
        WRITE_CSR(mkeycfg, reg64);
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] last_req_accepted set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_cfg_succesd(&reg64) && i < LOOP_MAX; ++ i) 
    {
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] cfg_succesd set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    sbi_printf("[IIE CVM Monitor@%s] Configure tweak key successfully. \r\n", __func__);

    return 0;
}

int config_key_id_with_rng(uint64_t keyid)
{
    uint64_t reg64 = READ_CSR(mkeycfg);
    for(int i = 0; (!read_key_expansion_idle(&reg64) || !read_random_ready_flag(&reg64))  && i < LOOP_MAX; ++ i)
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_last_req_accepted(&reg64) && i < LOOP_MAX; ++ i) 
    {
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        write_mode(&reg64, 0b10);
        write_tweak_flag(&reg64, 0b0);
        write_key_init_req(&reg64, 0b1);
        write_key_id(&reg64, keyid);
        WRITE_CSR(mkeycfg, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] last_req_accepted set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    for(int i = 0; !read_cfg_succesd(&reg64) && i < LOOP_MAX; ++ i) 
    {
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] cfg_succesd set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    sbi_printf("[IIE CVM Monitor@%s] Configure tweak key successfully. \r\n", __func__);

    return 0;
}

int config_key_id_with_sw(uint64_t keyid)
{
    uint64_t reg64 = READ_CSR(mkeycfg);
    for(int i = 0; !read_key_expansion_idle(&reg64) && i < LOOP_MAX; ++ i) 
    {
        sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        reg64 = READ_CSR(mkeycfg);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_last_req_accepted(&reg64) && i < LOOP_MAX; ++ i)
    {
        write_mode(&reg64, 0b01);
        write_tweak_flag(&reg64, 0b0);
        write_key_init_req(&reg64, 0b1);
        write_key_id(&reg64, keyid);
        WRITE_CSR(mkeycfg, reg64);
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] last_req_accepted hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] last_req_accepted set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }

    for(int i = 0; !read_cfg_succesd(&reg64) && i < LOOP_MAX; ++ i) 
    {
        reg64 = READ_CSR(mkeycfg);
        sbi_printf("[IIE CVM Monitor@%s] cfg_succesd hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
        if(i + 1 == LOOP_MAX)
        {
            sbi_printf("[IIE CVM Monitor@%s] cfg_succesd set failed!. mkeycfg %lx. \r\n", __func__, reg64);
            return -1;
        }
    }
    
    sbi_printf("[IIE CVM Monitor@%s] Configure tweak key successfully. \r\n", __func__);

    return 0;
}

uint64_t gen_key_id()
{
    /* TODO */
    return 0;
    
    uint64_t cnt = 0;
    do {
        if(++ last_key_id >= KEY_ID_NUM) last_key_id = 1;
        ++ cnt;
        if(cnt >= KEY_ID_NUM) return -1UL;
    }while(key_id_used[last_key_id]);
    key_id_used[last_key_id] = true;
    return last_key_id;
}

inline void release_key_id(uint64_t keyid)
{
    key_id_used[keyid] = false;
}


int test_cfg()
{
    // /* write random_ready_flag */
    // write_pos_k_bits(&key_regs->mkeycfg, 32, 1, 0b1);
    // /* write key_expansion_idle */
    // write_pos_k_bits(&key_regs->mkeycfg, 33, 1, 0b1);
    // /* read tweak_flag & key_init_req */
    // while(!read_tweak_flag(&key_regs->mkeycfg) || !read_pos_k_bits(&key_regs->mkeycfg, 63, 1)) 
    // {
    //     sbi_printf("[IIE CVM Monitor@%s] tweak_flag or key_init_req hasn't been set. mkeycfg %lx. \r\n", __func__, key_regs->mkeycfg);
    // }
    // /* set last_req_accepted */
    // write_pos_k_bits(&key_regs->mkeycfg, 34, 1, 0b1);
    // /* set cfg_succesd */
    // write_pos_k_bits(&key_regs->mkeycfg, 35, 1, 0b1);

    // sbi_printf("[IIE CVM Monitor@%s] test_cfg finished. \r\n", __func__);
    
    
    /* write random_ready_flag */
    // uint64_t reg64 = READ_CSR(mkeycfg);
    // sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);
    // write_pos_k_bits(&key_regs->mkeycfg, 32, 1, 0b1);
    // /* write key_expansion_idle */
    // write_pos_k_bits(&key_regs->mkeycfg, 33, 1, 0b1);
    // reg64 = READ_CSR(mkeycfg);
    // sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle or random_ready_flag hasn't been set. mkeycfg %lx. \r\n", __func__, reg64);

    return 0;
}