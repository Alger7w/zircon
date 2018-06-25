// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>

#include <block-client/client.h>
#include <blk-op/blk-op.h>
#include <zircon/syscalls.h>
#include <zircon/device/block.h>
#include <zircon/misc/xorshiftrand.h>
#include <zircon/process.h>
#include <sync/completion.h>


static void usage(void) {
    fprintf(stderr, "usage: blocklatency <args> <device>\n"
        "\n"
        "For all <num> use k/K, m/M, and g/G for ease of use/conversion\n"
        "args:  -wt <num> <num>  wait time is_wait_range in nanoseconds between operations {default: 10-100ns}\n"
        "       -it <num>        number of operations to perform {default: 100}\n"
        "       -tc <num>        number of threads to run on {default: 4}\n"
        "       -ts <num>        transfer size in bytes {default: 16M}\n"
        "       -mo <num>        maximum outstanding ops (1..default: 128)\n"
        "       -read            only performs reads {default}\n"
        "       -write           only performs writes\n"
        "       -read-write      alternates between read and write randomly\n"
        "       -linear <num>    transfers in linear order starting at specified block offset\n"
        "       -random          transfers in random order {default}\n"
        );

}

#define DEBUG(x...) fprintf(stderr, x)

//Macros for using str_to_number()
#define NUMMODE 1000    //used for standard orders of 1000 (seconds)
#define BYTEMODE 1024    //used for number of bytes

#define nextarg() do { argc--; argv++; } while (0)

//n is which index of argv has the parameter relative to current place
#define needparam(n) do { \
    argc--; argv++; \
    if (argc == 0) { \
        fprintf(stderr, "error: option %s needs one or more parameters\n", argv[n]); \
        usage(); \
        return -1; \
    } } while (0)

#define error(x...) do { fprintf(stderr, x); usage(); return -1; } while (0)

typedef struct {
    blkdev_t* blk;
    size_t transfer_size;
    size_t thread_count;
    uint64_t iters;
    zx_duration_t lbound;
    zx_duration_t ubound;
    uint64_t seed;
    int max_pending;
    bool linear;
    uint64_t offset;
    uint8_t operation;
    atomic_int pending;
} latency_args_t;

static completion_t* completions;
static latency_args_t largs;
static void* buf;
static const uint64_t bufsz = 8*1024*1024;

static uint64_t str_to_number(const char* str, uint16_t thousand) {
    char* end;
    uint64_t n = strtoull(str, &end, 10);

    uint64_t m = 1;
    switch (*end) {
    case 'G':
    case 'g':
        m = thousand*thousand*thousand;
        break;
    case 'M':
    case 'm':
        m = thousand*thousand;
        break;
    case 'K':
    case 'k':
        m = thousand;
        break;
    }
    return m * n;
}

static void nsec_to_str(uint64_t nanos) {
    float ftime;
    const char* unit;
    if (nanos > 1000*1000*1000) {
        unit = "seconds";
        ftime = nanos / (1000.0*1000.0*1000.0);
    } else if (nanos > 1000*1000) {
        unit = "millis";
        ftime = nanos / (1000.0*1000.0);
    } else if (nanos > 1000) {
        unit = "micros";
        ftime = nanos / 1000.0;
    } else {
        unit = "nanos";
        ftime = (float) nanos;
    }
    fprintf(stderr, "%07.3f %s", ftime, unit);
}

static void fill_buffer(rand64_t seed, uint64_t len) {
    uint64_t i;
    uint64_t* buffer = buf;
    for(i = 0; i < len/8; i++) {
        buffer[i] = rand64(&seed);
    }
}

static void print_buffer(uint64_t begin, uint64_t end) {
    uint64_t i, k = begin;
    uint8_t* buffer = buf;
    while(k < end) {
        for(i = 0; i < 66; i++) {
            fprintf(stderr, "%03d ", buffer[k]);
            if(++k >= end) break;
        }
        fprintf(stderr, "\n");
    }
}

size_t read_cline_args(int argc, char** argv, char** dev_name) {
    zx_status_t r = ZX_OK;
    while (argc > 0) {
        if (argv[0][0] != '-') {
            break;
        }
        if (!strcmp(argv[0], "-tc")) {
            needparam(-1);
            largs.thread_count = str_to_number(argv[0], NUMMODE);
        } else if (!strcmp(argv[0], "-ts")) {
            needparam(-1);
            largs.transfer_size = str_to_number(argv[0], BYTEMODE);
        } else if (!strcmp(argv[0], "-mo")) {
            needparam(-1);
            size_t n = str_to_number(argv[0], NUMMODE);
            if ((n < 1) || (n > 128)) {
                error("error: max pending must be between 1 and 128\n");
                return 0;
            }
            largs.max_pending = n;
        } else if(!(strcmp(argv[0], "-it"))) { //number of iterations
            needparam(-1);
            largs.iters = str_to_number(argv[0], NUMMODE);
        } else if(!(strcmp(argv[0], "-wt"))) {  //range of times to wait in between trials
            needparam(-1);
            largs.lbound = str_to_number(argv[0], NUMMODE);
            needparam(-2);
            largs.ubound = str_to_number(argv[0], NUMMODE);
            if(largs.lbound > largs.ubound) {       //incorrect ordering of bounds
                uint64_t temp = largs.lbound;
                largs.lbound = largs.ubound;
                largs.ubound = temp;
            }
        } else if (!strcmp(argv[0], "-read")) {
            largs.operation = BLOCKIO_READ;
        } else if (!strcmp(argv[0], "-write")) {
            largs.operation = BLOCKIO_WRITE;
        } else if (!strcmp(argv[0], "-read-write")) {
            largs.operation = BLOCKIO_READ | BLOCKIO_WRITE;
        } else if (!strcmp(argv[0], "-linear")) {
            largs.linear = true;
            needparam(-1);
            largs.offset = str_to_number(argv[0], NUMMODE);
        } else if (!strcmp(argv[0], "-random")) {
            largs.linear = false;
        } else if (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
            usage();
            return 0;
        } else {
            error("error: unknown option: %s\n", argv[0]);
        }
        nextarg();
    }
    if (argc == 0) {
        error("error: no device specified\n");
    }
    if (argc > 1) {
        error("error: unexpected arguments\n");
    }

    *dev_name = argv[0];    //path to device or device name

    return r;
}

//Sets up blk for use as well as reads other arguments
static int setup_blkdev(int argc, char** argv, fifo_client_t** client) {
    nextarg();
    char* dev_name[50];       //path to device or device name

    zx_status_t r = read_cline_args(argc, argv, dev_name);

    if (r != ZX_OK) return r;

    int fd;
    
    if ((fd = open(*dev_name, O_RDONLY)) < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", *dev_name);
        return -1;
    }
    if ((r = blkdev_open(fd, *dev_name, bufsz, largs.blk, client, &buf)) != ZX_OK) {
        fprintf(stderr, "error: cannot open '%s' %d\n", *dev_name, r);
        return -1;
    }
    size_t devtotal = largs.blk->info.block_count * largs.blk->info.block_size;

    if((largs.offset * largs.blk->info.block_size) + largs.transfer_size > devtotal) {
        error("Offset and length will go out of bounds\n");
    }
    
    if (largs.transfer_size % largs.blk->info.block_size) {
        error("Transfer size must be multiple of block_size\n");
    }
    // default to entire device
    if ((largs.transfer_size == 0) || (largs.transfer_size > devtotal)) {
        largs.transfer_size = devtotal;
    }
    return 0;
}

///Thread for performing block_fifo_txns
/**
 * Divides a transaction into base_request->length / vmo_size txns
 */
static int block_thread_io(void* arg) {
    zx_status_t status;
    block_thread_args_t* bthread = arg;
    block_fifo_request_t* base_request = bthread->req;
    fifo_client_t* client = bthread->client;
    size_t vmo_size;
    zx_vmo_get_size(largs.blk->vmo, &vmo_size);
    
    uint64_t num_txns = (base_request->length * largs.blk->info.block_size) / vmo_size;
    num_txns += (base_request->length * largs.blk->info.block_size) % vmo_size ? 1 : 0; //any remainder means one more txn
    block_fifo_request_t* reqs = malloc(num_txns * sizeof(block_fifo_request_t));
    
    uint64_t i;
    
    for(i = 0; i < num_txns; i++) {
        reqs[i] = *base_request;
        reqs[i].length = base_request->length / num_txns;
        base_request->dev_offset += reqs[i].length;
    }
    uint64_t num_todo = num_txns, offset = 0;
    while(num_todo > 0) {
        uint64_t this_iter = num_todo;
        //if we can't do them all this iteration, try the remaining amount
        while(atomic_fetch_add(&largs.pending, this_iter) > largs.max_pending) {
            atomic_fetch_sub(&largs.pending, this_iter);
            this_iter = largs.max_pending - largs.pending;
            fprintf(stderr, "Too many outstanding\n");
        }

        if((status = block_fifo_txn(client, &reqs[offset], this_iter)) != ZX_OK) {
            fprintf(stderr, "Error during transaction: %d\n", status);
        }
        num_todo -= this_iter;
        offset += this_iter;
        atomic_fetch_sub(&largs.pending, this_iter);
    }
    PUT(reqs->txnid);
    completion_signal(&completions[bthread->thrd_num]);
    free(reqs);
    return 0;
}

///Takes requests and distributes them across threads - one request object per thread
static zx_status_t block_io(fifo_client_t* client, block_fifo_request_t* requests, rand64_t* seed) {
    uint64_t cur_thread;
    thrd_t* thrds = malloc(largs.thread_count * sizeof(zx_handle_t));
    completions = malloc(largs.thread_count * sizeof(completion_t));
    block_thread_args_t* bt = malloc(largs.thread_count * sizeof(block_thread_args_t));
    //loop for number of operations calculated in setup_blkdev
    for(cur_thread = 0; cur_thread < largs.thread_count; cur_thread++) {     
        bt[cur_thread].req = &requests[cur_thread];
        bt[cur_thread].thrd_num = cur_thread;
        bt[cur_thread].client = client;
        completions[cur_thread] = COMPLETION_INIT;
        thrd_create(&thrds[cur_thread], block_thread_io, &bt[cur_thread]);
    }
    for(cur_thread = 0; cur_thread < largs.thread_count; cur_thread++) {   
        if((completion_wait_deadline(&completions[cur_thread], zx_deadline_after(ZX_SEC(10)))) != ZX_OK) {
            fprintf(stderr, "Timed out on Thread_num: %lu\n", cur_thread);
        }
        completion_reset(&completions[cur_thread]);
    }
    free(thrds);
    free(bt);
    free(completions);
    return ZX_OK;
}

///Initializes requests with command line arguments in largs
static void setup_requests(block_fifo_request_t* requests, rand64_t* seed) {
    bool alternate_rw = largs.operation == (BLOCKIO_READ | BLOCKIO_WRITE);
    uint64_t txn_blocks_total = largs.transfer_size / largs.blk->info.block_size;
    uint64_t length_per_thread = txn_blocks_total / largs.thread_count;
    //some threads will have a larger transfer size
    uint64_t remainders = txn_blocks_total % largs.thread_count;

    size_t dev_block_count = largs.blk->info.block_count;  //number of total blocks

    requests[0].vmoid = largs.blk->vmoid;
    requests[0].opcode = alternate_rw ? BLOCKIO_READ : largs.operation;   //if we are alternating, start on read
    requests[0].txnid = GET();
    requests[0].length = length_per_thread;
    if(remainders) {                //some threads will have one more block to transfer
        requests[0].length += 1;
        remainders--;
    }
    if(largs.linear) {
        requests[0].vmo_offset = 0;        //number of blocks offset for vmo
        requests[0].dev_offset = largs.offset;         //number of blocks offset for dev
    } else {
        uint64_t num = rand64(seed);
        requests[0].vmo_offset = 0;
        if(dev_block_count - txn_blocks_total <= 0) {
            requests[0].dev_offset = 0;
        } else {
            requests[0].dev_offset = num % (dev_block_count - txn_blocks_total); //number of blocks offset for dev, account for OOB
        }
    }
    uint64_t i;
    for(i = 1; i < largs.thread_count; i++) {
        requests[i].txnid = GET();
        requests[i].vmoid = largs.blk->vmoid;
        requests[i].opcode = alternate_rw ? BLOCKIO_READ : largs.operation;   //if we are alternating, start on read
        requests[i].length = length_per_thread;    //will be changed in linear_io/random_io
        if(remainders) {                //some threads will have one more block to transfer
            requests[i].length += 1;
            remainders--;
        }
        requests[i].vmo_offset = 0;
        requests[i].dev_offset = requests[i - 1].dev_offset + requests[i - 1].length;         //number of blocks offset for dev
    }
}

///Performs read/write requests on a block device determined and with arguments determined by read_cline_args with 
/**
 * Will perform random or linear transactions with arguments in "a" determining length,
 * offset on the device, combinations of read and write, and wait times between operations
 */
static zx_status_t run_latency_trials(fifo_client_t* client) {
    zx_duration_t wait = 0;
    zx_status_t r = 0;
    bool is_wait_range = true;
    if (largs.lbound == largs.ubound) {   //no wait range specified, take constant wait time
        wait = largs.lbound;
        is_wait_range = false;
    }
    bool alternate_rw = largs.operation == (BLOCKIO_READ | BLOCKIO_WRITE);
    zx_time_t prev = 0;
    block_fifo_request_t* requests = (block_fifo_request_t*) malloc(largs.thread_count * sizeof(block_fifo_request_t));

    rand64_t seed = RAND63SEED(largs.seed);

    uint64_t i;
    for (i = 0; i < largs.iters; i++) {
        setup_requests(requests, &seed);
        if (is_wait_range) {
            wait = (rand64(&seed) % (largs.ubound - largs.lbound)) + largs.lbound;
        }

        fill_buffer(seed, bufsz);     //Fill with random numbers
        prev = zx_clock_get(ZX_CLOCK_MONOTONIC);    //for recording time per iteration
        r = block_io(client, requests, &seed);
        if(r != ZX_OK) {
            fprintf(stderr, "Errno: %d\n", r);
        }
        nsec_to_str(zx_clock_get(ZX_CLOCK_MONOTONIC) - prev);
        fprintf(stderr, "\n");

        if(alternate_rw && (rand64(&seed) % 2)) {
            requests[0].opcode ^= BLOCKIO_READ | BLOCKIO_WRITE; //switch between read and write randomly
        }

        zx_nanosleep(zx_deadline_after(ZX_NSEC(wait)));
    }
    return ZX_OK;
    free(requests);
}

int main(int argc, char** argv) {
    zx_status_t r = ZX_OK;
    fifo_client_t* client;
    blkdev_t blk;
    latency_args_t tmp = {   //default settings
        .blk = &blk,
        .thread_count = 4,
        .transfer_size = 16*1024*1024,
        .iters = 100,
        .lbound = 10,
        .ubound = 100,
        .operation = BLOCKIO_READ,
        .seed = 7891263897612ULL,
        .max_pending = 128,
        .pending = 0,
        .linear = false,
    };
    memcpy(&largs, &tmp, sizeof(latency_args_t));
    if(setup_blkdev(argc, argv, &client) < 0) return 0;
    if((r = run_latency_trials(client)) != ZX_OK) {
        fprintf(stderr, "Error #: %d\n", r);
    }
    blkdev_close(largs.blk, client);
    return 0;
}
