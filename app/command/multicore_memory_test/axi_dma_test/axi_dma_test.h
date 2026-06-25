#ifndef __AXI_DMA_TEST_H__
#define __AXI_DMA_TEST_H__

int axi_dma_test_1bytes_handler(char *name);
int axi_dma_test_2bytes_handler(char *name);
int axi_dma_test_4bytes_handler(char *name);
int axi_dma_test_8bytes_handler(char *name);
int axi_dma_test_16bytes_handler(char *name);
int axi_dma_test_32bytes_handler(char *name);
int axi_dma_test_64bytes_handler(char *name);
int axi_dma_test_128bytes_handler(char *name);
int axi_dma_test_4Kbytes_handler(char *name);
int axi_dma_test_8Kbytes_handler(char *name);
int axi_dma_test_fix(char *name, int _size,
		     unsigned long fix_src, unsigned long fix_dst);

#endif
