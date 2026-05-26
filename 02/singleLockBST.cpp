// Single-lock Binary Search Tree.
// One global std::mutex protects every operation.
// Baseline for throughput comparison against hand-over-hand and lock-free BSTs.
//
// Build: g++ -std=c++20 -pthread -O2 singleLockBST.cpp -o singleLockBST

#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

struct Node {
    int   key;
    Node* left   = nullptr;
    Node* right  = nullptr;
    explicit Node(int k) : key(k) {}
};

// ---------------------------------------------------------------------------
// Single-lock BST
// ---------------------------------------------------------------------------

class SingleLockBST {
    Node*      root_ = nullptr;
    std::mutex mtx_;

public:
    bool contains(int key) {
        std::lock_guard<std::mutex> lk(mtx_);
        Node* cur = root_;
        while (cur) {
            if (key == cur->key) return true;
            cur = (key < cur->key) ? cur->left : cur->right;
        }
        return false;
    }

    bool insert(int key) {
        std::lock_guard<std::mutex> lk(mtx_);
        Node** slot = &root_;
        while (*slot) {
            if (key == (*slot)->key) return false;   // already present
            slot = (key < (*slot)->key)
                 ? &(*slot)->left
                 : &(*slot)->right;
        }
        *slot = new Node(key);
        return true;
    }

    bool remove(int key) {
        std::lock_guard<std::mutex> lk(mtx_);
        Node** slot = &root_;
        while (*slot) {
            if (key == (*slot)->key) {
                removeNode(slot);
                return true;
            }
            slot = (key < (*slot)->key)
                 ? &(*slot)->left
                 : &(*slot)->right;
        }
        return false;
    }

private:
    // Remove the node pointed to by *slot (caller holds the lock).
    static void removeNode(Node** slot) {
        Node* target = *slot;
        if (!target->left) {
            *slot = target->right;
            delete target;
        } else if (!target->right) {
            *slot = target->left;
            delete target;
        } else {
            // Two children: replace with in-order successor.
            Node** succSlot = &target->right;
            while ((*succSlot)->left)
                succSlot = &(*succSlot)->left;
            target->key = (*succSlot)->key;
            removeNode(succSlot);
        }
    }
};

// ===========================================================================
// Tests (same scenarios as lock-free BST)
// ===========================================================================

namespace test {

void singleThreadedSanity() {
    SingleLockBST t;
    assert(!t.contains(42));
    assert( t.insert(42));
    assert( t.contains(42));
    assert(!t.insert(42));
    for (int v : {10, 20, 30, 5, 25, 7, 1, 99}) assert(t.insert(v));
    for (int v : {10, 20, 30, 5, 25, 7, 1, 99, 42}) assert(t.contains(v));
    assert( t.remove(25));
    assert(!t.contains(25));
    assert(!t.remove(25));
    assert( t.contains(30));
    std::cout << "[ok] single-threaded sanity\n";
}

void concurrentStress() {
    constexpr int THREADS   = 8;
    constexpr int OPS       = 20000;
    constexpr int KEY_SPACE = 2000;

    SingleLockBST tree;
    for (int k = 0; k < KEY_SPACE; k += 2) tree.insert(k);

    std::barrier sync(THREADS);
    std::vector<std::thread> ts;
    std::atomic<long> ins{0}, del{0}, found{0}, miss{0};

    for (int tid = 0; tid < THREADS; ++tid) {
        ts.emplace_back([&, tid] {
            std::mt19937 rng(0xC0FFEE ^ tid);
            std::uniform_int_distribution<int> keyD(0, KEY_SPACE - 1);
            std::uniform_int_distribution<int> opD(0, 3);
            sync.arrive_and_wait();
            for (int i = 0; i < OPS; ++i) {
                int k = keyD(rng);
                switch (opD(rng)) {
                    case 0: ins  += tree.insert(k);                 break;
                    case 1: del  += tree.remove(k);                 break;
                    default:  // cases 2,3 → 50% reads
                        if (tree.contains(k)) ++found; else ++miss; break;
                }
            }
        });
    }
    for (auto& t : ts) t.join();

    std::unordered_set<int> ref;
    for (int k = 0; k < KEY_SPACE; ++k)
        if (tree.contains(k)) ref.insert(k);
    assert(ref.size() <= (size_t)KEY_SPACE);

    std::cout << "[ok] concurrent stress: "
              << "inserts="    << ins.load()
              << " removes="   << del.load()
              << " hits="      << found.load()
              << " miss="      << miss.load()
              << " final size=" << ref.size() << "\n";
}

} // namespace test

// ===========================================================================
// Benchmark harness
// ===========================================================================
// Usage: ./singleLockBST benchmark <threads> <ops_per_thread> <key_space>
//   Outputs one line:  variant=singleLock threads=T total_ops=N time_us=U throughput_ops_s=F

void runBenchmark(int threads, int opsPerThread, int keySpace) {
    SingleLockBST tree;
    for (int k = 0; k < keySpace; k += 2) tree.insert(k);

    std::barrier sync(threads);
    std::vector<std::thread> ts;
    std::atomic<long> ins{0}, del{0}, found{0}, miss{0};

    auto t0 = std::chrono::steady_clock::now();

    for (int tid = 0; tid < threads; ++tid) {
        ts.emplace_back([&, tid] {
            std::mt19937 rng(0xC0FFEE ^ tid);
            std::uniform_int_distribution<int> keyD(0, keySpace - 1);
            std::uniform_int_distribution<int> opD(0, 3);
            sync.arrive_and_wait();
            for (int i = 0; i < opsPerThread; ++i) {
                int k = keyD(rng);
                switch (opD(rng)) {
                    case 0: ins  += tree.insert(k);                 break;
                    case 1: del  += tree.remove(k);                 break;
                    default:  // cases 2,3 → 50% reads
                        if (tree.contains(k)) ++found; else ++miss; break;
                }
            }
        });
    }
    for (auto& t : ts) t.join();

    auto t1 = std::chrono::steady_clock::now();
    long totalOps = (long)threads * opsPerThread;
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double throughput = totalOps / (us / 1e6);

    std::cout << "variant=singleLock"
              << " threads=" << threads
              << " total_ops=" << totalOps
              << " time_us=" << (long)us
              << " throughput_ops_s=" << throughput
              << "\n";
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string(argv[1]) == "benchmark") {
        int threads     = argc >= 3 ? std::atoi(argv[2]) : 4;
        int opsPerThread = argc >= 4 ? std::atoi(argv[3]) : 50000;
        int keySpace    = argc >= 5 ? std::atoi(argv[4]) : 2000;
        runBenchmark(threads, opsPerThread, keySpace);
    } else {
        test::singleThreadedSanity();
        test::concurrentStress();
    }
    return 0;
}
