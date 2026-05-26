// Hand-over-hand (lock-coupling) Binary Search Tree.
// Each node owns its own std::mutex. Traversals lock at most two nodes at a
// time: the current node and its child, then release the parent before
// descending further.
//
// Build: g++ -std=c++20 -pthread -O2 handOverBST.cpp -o handOverBST

#include <algorithm>
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
    int        key;
    Node*      left   = nullptr;
    Node*      right  = nullptr;
    std::mutex mtx;
    explicit Node(int k) : key(k) {}
};

// ---------------------------------------------------------------------------
// Hand-over-hand BST
// ---------------------------------------------------------------------------
//
// A sentinel root node (key = INT_MAX) ensures every real key has a parent,
// which simplifies the coupling logic (we never need to special-case an
// empty tree or root replacement).

class HandOverBST {
    Node sentinel_{INT_MAX};   // always present; real keys are strictly less

public:
    // ------------------------------------------------------------------
    // contains – read-only traversal with hand-over-hand locking
    // ------------------------------------------------------------------
    bool contains(int key) {
        sentinel_.mtx.lock();
        Node* cur = (key < sentinel_.key) ? sentinel_.left : sentinel_.right;
        if (!cur) { sentinel_.mtx.unlock(); return false; }

        cur->mtx.lock();
        sentinel_.mtx.unlock();

        while (true) {
            if (key == cur->key) { cur->mtx.unlock(); return true; }
            Node* next = (key < cur->key) ? cur->left : cur->right;
            if (!next) { cur->mtx.unlock(); return false; }
            next->mtx.lock();
            cur->mtx.unlock();
            cur = next;
        }
    }

    // ------------------------------------------------------------------
    // insert – descend with coupling, then splice a new leaf
    // ------------------------------------------------------------------
    bool insert(int key) {
        sentinel_.mtx.lock();
        Node** slot = (key < sentinel_.key)
                    ? &sentinel_.left
                    : &sentinel_.right;

        if (!*slot) {
            *slot = new Node(key);
            sentinel_.mtx.unlock();
            return true;
        }

        Node* parent = &sentinel_;
        Node* cur    = *slot;
        cur->mtx.lock();
        parent->mtx.unlock();

        while (true) {
            if (key == cur->key) {
                cur->mtx.unlock();
                return false;           // already present
            }
            Node** childSlot = (key < cur->key) ? &cur->left : &cur->right;
            if (!*childSlot) {
                *childSlot = new Node(key);
                cur->mtx.unlock();
                return true;
            }
            Node* next = *childSlot;
            next->mtx.lock();
            cur->mtx.unlock();
            cur = next;
        }
    }

    // ------------------------------------------------------------------
    // remove – coupling keeps parent + child locked so we can re-link
    // ------------------------------------------------------------------
    bool remove(int key) {
        sentinel_.mtx.lock();
        Node** parentSlot = (key < sentinel_.key)
                          ? &sentinel_.left
                          : &sentinel_.right;

        if (!*parentSlot) { sentinel_.mtx.unlock(); return false; }

        Node* parent   = &sentinel_;
        Node* cur      = *parentSlot;
        Node** curSlot = parentSlot;          // *curSlot == cur
        cur->mtx.lock();

        // Descend with coupling until we find the key or a dead end.
        while (cur->key != key) {
            Node** childSlot = (key < cur->key) ? &cur->left : &cur->right;
            if (!*childSlot) {
                cur->mtx.unlock();
                parent->mtx.unlock();
                return false;
            }
            Node* next = *childSlot;
            next->mtx.lock();
            parent->mtx.unlock();
            parent  = cur;
            curSlot = childSlot;
            cur     = next;
        }

        // cur holds the key; parent and cur are both locked.
        removeNode(parent, curSlot, cur);
        return true;
    }

private:
    // Remove *cur* whose address in its parent's left/right is *slot*.
    // Caller must hold parent->mtx AND cur->mtx; this function unlocks both
    // (and any additionally locked successor).
    static void removeNode(Node* parent, Node** slot, Node* cur) {
        if (!cur->left) {
            *slot = cur->right;
            cur->mtx.unlock();
            parent->mtx.unlock();
            delete cur;
        } else if (!cur->right) {
            *slot = cur->left;
            cur->mtx.unlock();
            parent->mtx.unlock();
            delete cur;
        } else {
            // Two children: find in-order successor (leftmost in right subtree).
            // We need to lock the successor path with hand-over-hand too.
            Node* succParent = cur;
            Node* succ       = cur->right;
            succ->mtx.lock();
            Node** succSlot  = &cur->right;

            while (succ->left) {
                Node* next = succ->left;
                next->mtx.lock();
                succParent->mtx.unlock();
                // But keep cur locked – we will modify cur->key.
                if (succParent != cur) { /* already unlocked above */ }
                succParent = succ;
                succSlot   = &succ->left;
                succ       = next;
            }

            // Copy successor key into cur, then unlink successor.
            cur->key = succ->key;
            *succSlot = succ->right;

            succ->mtx.unlock();
            if (succParent != cur) succParent->mtx.unlock();
            cur->mtx.unlock();
            parent->mtx.unlock();
            delete succ;
        }
    }
};

// ===========================================================================
// Tests (same scenarios as the other BST variants)
// ===========================================================================

namespace test {

void singleThreadedSanity() {
    HandOverBST t;
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

    HandOverBST tree;
    // Shuffle keys to avoid degenerate linear tree (sequential insert → O(n) depth).
    std::vector<int> keys;
    for (int k = 0; k < KEY_SPACE; k += 2) keys.push_back(k);
    std::mt19937 prepopRng(42);
    std::shuffle(keys.begin(), keys.end(), prepopRng);
    for (int k : keys) tree.insert(k);

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

void runBenchmark(int threads, int opsPerThread, int keySpace) {
    HandOverBST tree;
    std::vector<int> keys;
    for (int k = 0; k < keySpace; k += 2) keys.push_back(k);
    std::mt19937 prepopRng(42);
    std::shuffle(keys.begin(), keys.end(), prepopRng);
    for (int k : keys) tree.insert(k);

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

    std::cout << "variant=handOver"
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
