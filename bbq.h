#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <algorithm>

#pragma once

namespace PEX {
namespace BBQ {
namespace SPSC {

#define bbq_likely(x)   (__builtin_expect(!!(x),true))
#define bbq_unlikely(x) (__builtin_expect(!!(x),false))
#define bbq_load_rlx(x) std::atomic_load_explicit(&x, std::memory_order_relaxed);
#define bbq_load_acq(x) std::atomic_load_explicit(&x, std::memory_order_acquire);
#define bbq_store_rlx(x, v) std::atomic_store_explicit(&x, v, std::memory_order_relaxed);
#define bbq_store_rel(x, v) std::atomic_store_explicit(&x, v, std::memory_order_release);

/* Block based queue with capacity of N and B blocks */
template<class T, size_t N, size_t B>
class Queue {

    /* Each block contains NE entries */
    static constexpr size_t NE = N / B;

    /* cache line size */
    static constexpr uint64_t CACHELINE_SIZE = 64;

    /* the number of version bits */
    static constexpr uint64_t VERSION_BITS = 44;

    /* the number of index bits */
    static constexpr uint64_t INDEX_BITS = 20;

    /* make sure parameters are valid */
    static_assert(NE < (1UL << INDEX_BITS), "too many entries in one block");
    static_assert(N % B == 0, "N % B must be 0");

    /* 64 bit Field, contains two segments, version and index */
    struct Field {
        Field() {}
        Field(uint64_t vsn, uint64_t idx) : version(vsn), index(idx) {}
        Field operator+(uint64_t n) {
            index += n;
            return *this;
        } 
        struct {
            uint64_t version : VERSION_BITS;
            uint64_t index : INDEX_BITS;
        };
    };

    /* block, contains NE entries */
    struct Block {
        Block(){}
        void init(uint64_t index) {
            Field f = Field(0, index);
            bbq_store_rlx(alloc, f);
            bbq_store_rlx(comm, f);
            bbq_store_rlx(resv, f);
            bbq_store_rlx(cons, f);
        }

        alignas(CACHELINE_SIZE) std::atomic<Field> alloc;
        alignas(CACHELINE_SIZE) std::atomic<Field> comm;
        alignas(CACHELINE_SIZE) std::atomic<Field> resv;
        alignas(CACHELINE_SIZE) std::atomic<Field> cons;
        alignas(CACHELINE_SIZE) T data[NE];

    } __attribute__((aligned(CACHELINE_SIZE)));

    enum RetStatus {NO_ENTRY, NOT_AVAILABLE, SUCCESS, BLOCK_DONE};

public:
    Queue() {
        blocks[0].init(0);
        for (uint64_t i = 1; i < B; i++) {
            blocks[i].init(NE);
        }
        Field f = Field(0, 0);
        bbq_store_rlx(phead, f);
        bbq_store_rlx(chead, f);
    }
    bool enqueue(T t) {
        while(true) {
            // get phead and block
            Field ph = bbq_load_rlx(phead);
            Block* b = &blocks[ph.index];

            std::pair<RetStatus, int> retval = allocate_entry(b);
            if (retval.first == SUCCESS) {
                commit_entry(b, retval.second, t);
                return true;
            } else {
                RetStatus ret = advance_phead(ph);
                if (ret == SUCCESS) {
                    continue;
                } else {
                    return false;
                }
            }
        }
    }
    bool dequeue(T& t) {
        while(true) {
            // get chead and block
            Field ch = bbq_load_rlx(chead);
            Block* b = &blocks[ch.index];

            std::pair<RetStatus, Field> retval = reserve_entry(b);
            // std::cout << "retstatus: " << retval.first << std::endl;
            if (retval.first == SUCCESS) {
                t = consume_entry(b, retval.second);
                return true;
                // if (t == NULL) {
                //     continue;
                // } else {
                //     return true;
                // }
            } else if (retval.first == NO_ENTRY) {
                return false;
            } else if (retval.first == NOT_AVAILABLE) {
                return false;
            } else {
                if (advance_chead(ch, retval.second.version)) {
                    continue;
                } else {
                    return false;
                }
            }
        }
    }

    void printData() {
        for (uint64_t i = 0; i < B; i++) {
            Field a = bbq_load_rlx(blocks[i].alloc);
            Field c = bbq_load_rlx(blocks[i].comm);
            std::cout << "a.index: " << a.index << std::endl;
            std::cout << "c.index: " << c.index << std::endl;
            std::cout << "block " << i + 1 << ": ";
            for (uint64_t j = 0; j < NE; j++) {
                std::cout << blocks[i].data[j] << " ";
            }
            std::cout << std::endl;
        }
    }

private:
    std::pair<RetStatus, int> allocate_entry(Block* b) {
        Field a = bbq_load_rlx(b->alloc)
        // std::cout << a.index << std::endl;
        if (a.index >= NE) {
            return std::make_pair(BLOCK_DONE, -1);
        }
        // should be fetch and add
        Field old_alloc = bbq_load_acq(b->alloc);
        int old = old_alloc.index;
        bbq_store_rel(b->alloc, old_alloc + 1);
        // faa end
        if (old >= NE) {
            return std::make_pair(BLOCK_DONE, -1);
        }
        return std::make_pair(SUCCESS, old);
    }

    void commit_entry(Block* b, int index, T t) {
        b->data[index] = t;
        // should be atomic add
        Field c = bbq_load_acq(b->comm);
        bbq_store_rel(b->comm, c + 1);
        // add end
    }

    RetStatus advance_phead(Field ph) {
        // std::cout << "enq: entered prod_advance" << std::endl;
        // std::cout << "phead index: " << ph.index << std::endl;
        // std::cout << "phead version: " << ph.version << std::endl;
        Block* nb = &blocks[(ph.index + 1) % B];
        Field c = bbq_load_rlx(nb->cons);
        // std::cout << "c.version: " << c.version << std::endl;
        // std::cout << "c.index: " << c.index << std::endl;
        // std::cout << "ph.version: " << ph.version << std::endl;
        // std::cout << "ph.index: " << ph.index << std::endl;
        if (c.version < ph.version || (c.version == ph.version && c.index != NE)) {
            Field r = bbq_load_rlx(nb->resv);
            if (r.index == c.index) {
                return NO_ENTRY;
            } else {
                return NOT_AVAILABLE;
            }
        }
        Field f = Field((ph.version + 1), 0);
        // should be atomic max
        Field a = bbq_load_rlx(nb->alloc);
        if (f.version > a.version) {
            bbq_store_rlx(nb->alloc, f);
        }
        // atomic max end

        // should be atomic max
        Field co = bbq_load_rlx(nb->comm);
        if (f.version > co.version) {
            bbq_store_rlx(nb->comm, f);
        }
        // atomic max end

        // should be atomic max
        ph + 1;
        if (ph.index >= NE) {
            ph.index = ph.index % NE;
            ph.version += 1;
        }
        bbq_store_rlx(phead, ph);
        // atomic max end

        // std::cout << "exit prod advance" << std::endl;
        return SUCCESS;
    }

    std::pair<RetStatus, Field> reserve_entry(Block* b) {
        // std::cout << "reserve entry entered" << std::endl;
        while (true) {
            Field r = bbq_load_rlx(b->resv);
            if (r.index < NE) {
                Field c = bbq_load_rlx(b->comm);
                if (r.index == c.index) {
                    return std::make_pair(NO_ENTRY, r); 
                }
                if (c.index != NE) {
                    Field a = bbq_load_rlx(b->alloc);
                    if (a.index != c.index) {
                        return std::make_pair(NOT_AVAILABLE, r);
                    }
                }

                // should be atomic max
                Field res = bbq_load_rlx(b->resv);
                int max = std::max((int)res.index, (int)(r.index + 1));
                Field newresv = Field(r.version, max);
                Field oldres = bbq_load_rlx(b->resv);
                bbq_store_rlx(b->resv, newresv);
                if (oldres.index == r.index) {
                // atomic max end
                    return std::make_pair(SUCCESS, r);
                } else {
                    std::cout << "IN HERE" << std::endl;
                    continue;
                }
            }
            std::cout << "block done" << std::endl;
            return std::make_pair(BLOCK_DONE, r);
        }
    }

    T consume_entry(Block* b, Field f) {
        // std::cout << "consume entry entered" << std::endl;
        T data = b->data[f.index];
        b->data[f.index] = 0; //FOR TESTING PURPOSES ONLY
        // should be atomic add
        Field c = bbq_load_acq(b->cons);
        bbq_store_rel(b->cons, c + 1);
        // atomic add end
        return data;
    }

    bool advance_chead(Field ch, int version) {
        Block* nb = &blocks[(ch.index + 1) % B];
        Field c = bbq_load_rlx(nb->comm);
        if (c.version != ch.version + 1) {
            return false;
        }
        Field f = Field((ch.version + 1), 0);
        // should be atomic max
        Field cons = bbq_load_rlx(nb->cons);
        if (f.version > cons.version) {
            bbq_store_rlx(nb->cons, f);
        }
        // atomic max end
        // should be atomic max
        Field resv = bbq_load_rlx(nb->resv);
        if (f.version > resv.version) {
            bbq_store_rlx(nb->resv, f);
        }
        // atomic max end
        // should be atomic max
        ch + 1;
        if (ch.index >= NE) {
            ch.index = ch.index % NE;
            ch.version += 1;
        }
        bbq_store_rlx(chead, ch);
        // atomic max end

        return true;
    }

private:
    alignas(CACHELINE_SIZE) Block blocks[B];
    alignas(CACHELINE_SIZE) std::atomic<Field> phead;
    alignas(CACHELINE_SIZE) std::atomic<Field> chead;
};

}
}
}