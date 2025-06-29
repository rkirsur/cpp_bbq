// Compiled with: g++ -O2 -std=c++11 main.cpp -I./
#include <cassert>
#include <pthread.h>
#include <iostream>
#include "bbq.h"
#include <stdint.h>
#include <chrono>
#include <stdlib.h>

static constexpr uint64_t ITERS = 16;
static constexpr uint64_t CAPACITY = 16;
static constexpr uint64_t NUM_OF_BLOCKS = 4;

PEX::BBQ::SPSC::Queue<uint64_t, CAPACITY, NUM_OF_BLOCKS> q;

void *writer(void *arg)
{
    for (uint64_t i = 0; i < 2; i++) {
        while(!q.enqueue(*(uint64_t *)arg)); // enqueue data i
        // q.enqueue(*(uint64_t *)arg);
    }
    // (void)arg;
    return NULL;
}

void *reader(void *arg)
{
    uint64_t buf;
    for (uint64_t i = 0; i < ITERS; i++) {
        while(!q.dequeue(buf)); // get data i
        if (buf != i) abort();
    }
    (void)arg;
    return NULL;
}

int main(void)
{
    auto begin = std::chrono::steady_clock::now();

    uint64_t arg1 = 1;
    uint64_t arg2 = 2;
    pthread_t t_writer1;
    pthread_t t_writer2;
    // pthread_t t_reader;
    pthread_create(&t_writer1, NULL, writer, &arg1);
    pthread_create(&t_writer2, NULL, writer, &arg2);
    // pthread_create(&t_reader, NULL, reader, NULL);
    // pthread_join(t_reader, NULL);
    pthread_join(t_writer1, NULL);
    pthread_join(t_writer2, NULL);

    q.printData();

    auto end = std::chrono::steady_clock::now();

    float time_in_sec = std::chrono::duration<double, std::milli>(end - begin).count() / 1000.0;
    uint64_t total_op = ITERS * 2; // producer's and consumer's
    std::cout << "SPSC BBQ: finish writing and reading with throughput = " << total_op / time_in_sec << " op/s.\n";
    return 0;
}
