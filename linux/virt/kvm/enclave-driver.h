#ifndef _CVM_ENCLAVE_DRIVER
#define _CVM_ENCLAVE_DRIVER

#define SBI_EXT_CVM 0x20000217
#define SBI_EXT_CVM_PRINT 0x0
#define SBI_EXT_CVM_LOAD 0x7

//add load file struct
// struct load_file{
// 	unsigned long src_hva;
// 	unsigned long des_gpa;
// 	unsigned long file_size;
// };

#endif