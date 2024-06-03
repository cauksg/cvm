#ifndef _ATTEST_H
#define _ATTEST_H

#include "sm/enclave.h"
#include <sbi/sbi_cvm.h>

void attest_init();

void hash_enclave(struct enclave_t* enclave, void* hash, uintptr_t nonce);

void update_enclave_hash(char *output, void* hash, uintptr_t nonce_arg);

void sign_enclave(void* signature, unsigned char *message, int len);

int verify_enclave(void* signature, unsigned char *message, int len);

/* IIE CVM attestation */
void hash_cvm(struct sbi_cvm* cvm, void* hash, uintptr_t nonce);
int attest_cvm(unsigned long vmid, uintptr_t report_ptr, uintptr_t nonce);


#endif /* _ATTEST_H */
