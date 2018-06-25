
#ifndef __BLK_OP_H
#define __BLK_OP_H

#include <stdio.h>
#include <stdlib.h>

#include <zircon/device/block.h>
#include <zircon/types.h>
#include <sync/completion.h>
#include <block-client/client.h>


__BEGIN_CDECLS

typedef struct {
    int fd;
    zx_handle_t vmo;
    zx_handle_t fifo;
    txnid_t txnid;
    vmoid_t vmoid;
    size_t bufsz;
    block_info_t info;
} blkdev_t;

typedef struct {
    fifo_client_t* client;
    block_fifo_request_t* req;
    uint64_t thrd_num;
} block_thread_args_t;

txnid_t GET(void);

void PUT(uint64_t n);

///Closes the specified device and client
void blkdev_close(blkdev_t* blk, fifo_client_t* client);

///Opens a block device into blk, and a fifo_client_t in client
/**
 * 
 * @param fd Is the file descriptor number of the device to be opened
 * @param dev Is the path to the device
 * @param bufsz Size of buffer to map vmar to in bytes
 * @param blk Struct to contain relevant information for block device
 * @param client The fifo_client_t to facilitate I/O operations on block device
 * @param buf_addr Address of the buffer being mapped
 */
zx_status_t blkdev_open(int fd, const char* dev, size_t bufsz,
                blkdev_t* blk, fifo_client_t** client, void** buf);

__END_CDECLS

#endif
