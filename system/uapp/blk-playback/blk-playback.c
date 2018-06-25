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


#define MAX_THREADS 100

#define BUFSZ 1024*1024*1024

typedef struct trace_req {
    uint64_t tid;
    block_fifo_request_t* req;
    uint64_t nanos;

} trace_req_t;

static trace_req_t* treqs;  //haha
static trace_req_t* treqs_thread[MAX_THREADS];    //each pointer will point to the beginning of a subsection of treqs
static size_t num_treqs_thread[MAX_THREADS];    //number of reqs per thread

// List of treqs
// Note that tid is mostly arbitrary
// ------tid------time--------
// ------000------1000--------  <<< treqs_thread[0] points here - num_treqs_thread[0] = 3
// ------000------1005--------
// ------000------1023--------
// ------001------0500--------  <<< treqs_thread[1] points here - num_treqs_thread[1] = 1
// ------007------3450--------  <<< treqs_thread[2] points here - num_treqs_thread[2] = 4
// ------007------3700--------
// ------007------4000--------
// ------007------4200--------
// ------010------4200--------  <<< treqs_thread[3] points here
// continuing...

static size_t num_reqs_total;
static size_t num_threads;
static rand64_t seed = RAND63SEED(7234509);
static void* buf;
static fifo_client_t* client;
static blkdev_t* blk;
static atomic_ulong done_wait;

static void usage(void) {
    fprintf(stdout, "blk-playback <input_file_path> <device_path>\n");
}

static void free_treqs(uint64_t num_to_clean) {
    uint64_t i;
    for (i = 0; i < num_to_clean; i++) {
        free(treqs[i].req);
    }
    free(treqs);
}

//For sorting by thread id
int compare_treq_tid(const void* a, const void* b) {
    uint64_t l = ((trace_req_t*)a)->tid;
    uint64_t r = ((trace_req_t*)b)->tid;
    return l - r;
}

//For sorting by timestamp
int compare_treq_ts(const void* a, const void* b) {
    uint64_t l = ((trace_req_t*)a)->nanos;
    uint64_t r = ((trace_req_t*)b)->nanos;
    return l - r;
}


//assumes that treqs are sorted ascending by ts already
static void convert_ts_to_offset(void) {
    uint64_t i, start = treqs[0].nanos;     //earliest req is defined as t = 0ns
    for (i = 0; i < num_reqs_total; i++) {
        treqs[i].nanos -= start;
    }
}

static int make_dummy_file(const char* file_name) {
    FILE* of = fopen(file_name, "w");
    if(!of) {
        fprintf(stderr, "Cannot open file %s\n", file_name);
        return -1;
    }
    uint64_t i, num_reqs = 50;
    for(i = 0; i < num_reqs; i++) {
        fprintf(of, "%s,", "block");
        fprintf(of, "%lu,", rand64(&seed) % 99999999); //ts
        fprintf(of, "%lu,", rand64(&seed) % 3000000); //dev_off
        fprintf(of, "%lu,", rand64(&seed) % 140000); //req_length
        fprintf(of, "%lu\n", rand64(&seed) % 7);    //limit thread ids for testing
    }
    fclose(of);
    return 0;
}

static int run_requests_thread(void* arg) {
    uint64_t thread_index = *(uint64_t*)arg;
    size_t num_reqs = num_treqs_thread[thread_index];
    trace_req_t* trace_req = treqs_thread[thread_index];
    block_fifo_request_t* req;
    zx_status_t status = ZX_OK;
    uint64_t i;
    uint64_t txn_id = GET();

    while(!atomic_load(&done_wait));
    for (i = 0; i < num_reqs; i++) {
        req = trace_req->req;
        req->txnid = txn_id;
        zx_nanosleep(zx_deadline_after(ZX_NSEC(trace_req->nanos)));     //wait until it's time
        if((status = block_fifo_txn(client, req, 1)) != ZX_OK) {
            fprintf(stderr, "failed reqno: %lu on thread %lu error: %d\n", i, thread_index, status);
        }
        trace_req++;
    }
    PUT(txn_id);
    atomic_fetch_add(&done_wait, 1);    //one thread more done
    return 0;
}

static int run_reqeusts(void) {
    thrd_t* threads = malloc(num_threads * sizeof(thrd_t));
    done_wait = 0;
    uint64_t i;
    for (i = 0; i < num_threads; i++) {
        fprintf(stdout, "creating thread: %lu\n", i);
        thrd_create(&threads[i], run_requests_thread, &i);
    }
    atomic_fetch_add(&done_wait, 1);
    while(atomic_load(&done_wait) <= num_threads) {
        zx_nanosleep(zx_deadline_after(ZX_SEC(1)));
        fprintf(stdout, "Waiting... on %lu more threads\n", num_threads + 1 - done_wait);
    }
    return 0;
}

static int parse_line(char* line, uint64_t index) {
    char* str = strtok(line, ",");  //TODO(zapatoshoe) handle type/name
    str = strtok(line, ",");
    treqs[index].nanos = strtoull(str, NULL, 10);   //timestamp
    treqs[index].req->dev_offset = strtoull(str, NULL, 10); //dev_off
    treqs[index].req->length = strtoull(str, NULL, 10); //req_length
    uint64_t req_end_pos = treqs[index].req->length + treqs[index].req->dev_offset;
    if(req_end_pos > blk->info.block_count) {
        fprintf(stderr, "request will go out of bounds at block: %lu\n", req_end_pos);
        return -1;
    }
    treqs[index].tid = strtoull(str, NULL, 10); //thread id

    treqs[index].req->vmoid = blk->vmoid;
    treqs[index].req->vmo_offset = 0;
    treqs[index].req->opcode = BLOCKIO_READ;    //TODO(zapatoshoe)
    return 0;
}

static int parse_file(FILE* inf) {
    //Read one line at a time, get ts, dev_off (blocks), length (blocks), tid
    //Create an array of each of these "requests"
    //Sort this array by tid
    //Split into subarrays for each thread and then sort by ts (first comes first)
    char c;
    char line[128]; //line should be up to 4 x 64 bit items plus 3 commas - <84 characters
    fgets(line, 128, inf);  //trash header line
    uint64_t i = 0;
    num_reqs_total = 0;
    while ((c = fgetc(inf)) != EOF) {
        if (c == '\n') num_reqs_total++; //num_reqs == num rows in file
    }
    treqs = malloc(num_reqs_total * sizeof(trace_req_t));
    rewind(inf);
    fgets(line, 128, inf);  //trash header line
    for (i = 0; i < num_reqs_total; i++) {
        fgets(line, 128, inf);
        treqs[i].req = malloc(sizeof(block_fifo_request_t));
        if(parse_line(line, i)) {
            goto fail;
        }
    }

    return 0;
fail:
    fprintf(stderr, "error occurred parsing request: %lu\n", i);
    free_treqs(i + 1);
    return -1;
}

static int configure_reqs(FILE* inf) {
    if(parse_file(inf)) return -1;
    qsort(treqs, num_reqs_total, sizeof(trace_req_t), compare_treq_ts);   //May already be sorted by default
    convert_ts_to_offset();
    qsort(treqs, num_reqs_total, sizeof(trace_req_t), compare_treq_tid);
    
    uint64_t i = 0;
    uint64_t prev_start_index = 0, prev_id = treqs[0].tid;
    num_threads = 1;        //num_threads always leads thread_index by 1
    treqs_thread[0] = &treqs[0];
    for(i = 1; i < num_reqs_total; i++) {
        if(treqs[i].tid != prev_id) {
            num_treqs_thread[num_threads - 1] = i - prev_start_index;   //set previous subsection length
            treqs_thread[num_threads] = &treqs[i];  //new start points at current req
            prev_start_index = i;
            prev_id = treqs[i].tid;
            num_threads++;
        }
    }
    num_treqs_thread[num_threads - 1] = i - prev_start_index;   //set last subsection
    for(i = 0; i < num_threads; i++) {
        qsort(treqs_thread[i], num_treqs_thread[i], sizeof(trace_req_t), compare_treq_ts);
    }

    return 0;
}

static int setup_blkdev(int argc, char** argv) {
    zx_status_t r = ZX_OK;
    int fd;
    
    if ((fd = open(argv[1], O_RDONLY)) < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return -1;
    }
    if ((r = blkdev_open(fd, argv[1], BUFSZ, blk, &client, &buf)) != ZX_OK) {
        fprintf(stderr, "error: cannot open '%s' %d\n", argv[1], r);
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    argv++; argc--; //ignore program name
    if (argc != 2) {
        fprintf(stdout, "too few or too many arguments\n\n");
        usage();
    }
    if(make_dummy_file(argv[0])) return 0;
    blkdev_t b;
    blk = &b;
    if(setup_blkdev(argc, argv)) return 0;
    FILE* inf = fopen(argv[0], "r");
    if (inf == NULL) {
        fprintf(stderr, "Failed opening file '%s'\n", argv[0]);
        exit(1);
    }
    configure_reqs(inf);
    run_reqeusts();
    free_treqs(num_reqs_total);
    blkdev_close(blk, client);
    fclose(inf);
}