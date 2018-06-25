#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <zircon/syscalls.h>
#include <zircon/device/block.h>
#include <zircon/misc/xorshiftrand.h>
#include <zircon/process.h>
#include <blk-op/blk-op.h>
#include <block-client/client.h>

static atomic_uint_fast64_t IDMAP0 = 0xFFFFFFFFFFFFFFFFULL;
static atomic_uint_fast64_t IDMAP1 = 0xFFFFFFFFFFFFFFFFULL;
txnid_t GET(void) {
    uint64_t n = __builtin_ffsll(atomic_load(&IDMAP0));
    if (n > 0) {
        n--;
        atomic_fetch_and(&IDMAP0, ~(1ULL << n));
        return n;
    } else if ((n = __builtin_ffsll(atomic_load(&IDMAP1))) > 0) {
        n--;
        atomic_fetch_and(&IDMAP1, ~(1ULL << n));
        return n + 64;
    } else {
        fprintf(stderr, "FATAL OUT OF IDS\n");
        sleep(100);
        exit(-1);
    }
}

void PUT(uint64_t n) {
    if (n > 127) {
        fprintf(stderr, "FATAL BAD ID %zu\n", n);
        sleep(100);
        exit(-1);
    }
    if (n > 63) {
        atomic_fetch_or(&IDMAP1, 1ULL << (n - 64));
    } else {
        atomic_fetch_or(&IDMAP0, 1ULL << n);
    }
}

void blkdev_close(blkdev_t* blk, fifo_client_t* client) {
    if (blk->fd >= 0) {
        close(blk->fd);
    }
    zx_handle_close(blk->vmo);
    zx_handle_close(blk->fifo);
    memset(blk, 0, sizeof(blkdev_t));
    block_fifo_release_client(client);
    blk->fd = -1;
}

zx_status_t blkdev_open(int fd, const char* dev, size_t bufsz,
                blkdev_t* blk, fifo_client_t** client, void** buf_addr) {
    memset(blk, 0, sizeof(blkdev_t));
    blk->fd = fd;
    blk->bufsz = bufsz;
    
    zx_status_t r = ZX_OK;
    if (ioctl_block_get_info(fd, &blk->info) != sizeof(block_info_t)) {
        fprintf(stderr, "error: cannot get block device info for '%s'\n", dev);
        goto fail;
    }
    if (ioctl_block_get_fifos(fd, &blk->fifo) != sizeof(zx_handle_t)) {
        fprintf(stderr, "error: cannot get fifo for '%s'\n", dev);
        goto fail;
    }
    if ((r = zx_vmo_create(bufsz, 0, &blk->vmo)) != ZX_OK) {
        fprintf(stderr, "error: out of memory %d\n", r);
        goto fail;
    }
    if((r = zx_vmar_map(zx_vmar_root_self(), 0, blk->vmo, 0, bufsz, ZX_VM_FLAG_PERM_READ | 
                ZX_VM_FLAG_PERM_WRITE, (uintptr_t*) buf_addr) != ZX_OK)) {
        fprintf(stderr, "error: failed to map vmo %d\n", r);
        goto fail;
    }

    for (uint8_t n = 0; n < 128; n++) {
        if (ioctl_block_alloc_txn(fd, &blk->txnid) != sizeof(txnid_t)) {
            fprintf(stderr, "error: cannot allocate txn for '%s'\n", dev);
            goto fail;
        }
        if (blk->txnid != n) {
            fprintf(stderr, "error: unexpected txid %u\n", blk->txnid);
            goto fail;
        }
    }

    zx_handle_t dup;
    if ((r = zx_handle_duplicate(blk->vmo, ZX_RIGHT_SAME_RIGHTS, &dup)) != ZX_OK) {
        fprintf(stderr, "error: cannot duplicate handle %d\n", r);
        goto fail;
    }
    if (ioctl_block_attach_vmo(fd, &dup, &blk->vmoid) != sizeof(vmoid_t)) {
        fprintf(stderr, "error: cannot attach vmo for '%s'\n", dev);
        goto fail;
    }

    if ((r = block_fifo_create_client(blk->fifo, client)) != ZX_OK) {
        fprintf(stderr, "error: cannot create client %d\n", r);
        goto fail;
    }

    return ZX_OK;
fail:
    blkdev_close(blk, NULL);
    return r == ZX_OK ? ZX_ERR_INTERNAL : r;
}
