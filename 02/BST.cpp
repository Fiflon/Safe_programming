#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>
#ifndef BST_TRACE
#define BST_TRACE 0
#endif

inline std::mutex& logMutex() { static std::mutex m; return m; }

inline std::string tidStr() {
    std::ostringstream os; os << std::this_thread::get_id();
    return os.str();
}

#if BST_TRACE >= 1
#  define LOG(msg) do {                                       \
        std::lock_guard<std::mutex> _lg(logMutex());          \
        std::cout << "[t" << tidStr() << "] " << msg << '\n'; \
    } while (0)
#else
#  define LOG(msg) do {} while (0)
#endif

#if BST_TRACE >= 2
#  define LOG2(msg) LOG(msg)
#else
#  define LOG2(msg) do {} while (0)
#endif

static volatile int workSink = 0;

static void simulateWork(int iters) {
    int x = 1;
    for (int i = 0; i < iters; ++i) x = x * 7 + i;
    workSink = x;
}


struct Node {
    const bool isLeaf;
    const int  key;
    Node(bool leaf, int k) : isLeaf(leaf), key(k) {}
    virtual ~Node() = default;
};

struct Leaf : Node {
    explicit Leaf(int k) : Node(true, k) {}
};

struct InternalNode;

struct alignas(4) Info { virtual ~Info() = default; };

struct InsertInfo : Info {
    InternalNode* p; // parent 
    InternalNode* newInternal; // new sub-tree to add
    Node*         l; // existing leaf to replace
};

struct DeleteInfo : Info {
    InternalNode* gp; // grandparent 
    InternalNode* p; // parent
    Node*         l; // leaf carrying the key to delete
    uintptr_t     pupdate; // observed value p->update AT SEARCH TIME
};

static_assert(alignof(Info) >= 4,
              "Info must provide at least 2 free low bits for tagged pointers");
static_assert((alignof(InsertInfo) & uintptr_t(0x3)) == 0,
              "InsertInfo alignment must keep low tag bits clear");
static_assert((alignof(DeleteInfo) & uintptr_t(0x3)) == 0,
              "DeleteInfo alignment must keep low tag bits clear");

enum : uintptr_t {
    CLEAN      = 0,
    DFLAG      = 1,
    IFLAG      = 2,
    MARK       = 3,
    STATE_MASK = 0x3,
    PTR_MASK   = ~uintptr_t(0x3)
};

static inline uintptr_t pack(Info* p, uintptr_t state) {
    return reinterpret_cast<uintptr_t>(p) | state;
}
static inline Info* getPtr(uintptr_t u) {
    return reinterpret_cast<Info*>(u & PTR_MASK);
}
static inline uintptr_t getState(uintptr_t u) {
    return u & STATE_MASK;
}

struct InternalNode : Node {
    std::atomic<Node*>     left;
    std::atomic<Node*>     right;
    std::atomic<uintptr_t> update;

    InternalNode(int k, Node* l, Node* r)
        : Node(false, k), left(l), right(r), update(CLEAN) {}
};

class LockFreeBST {
    static constexpr int INF1 = INT_MAX - 1;
    static constexpr int INF2 = INT_MAX;

    InternalNode* root;

    struct SearchRes {
        InternalNode* gp; // grandparent 
        InternalNode* p; // parent
        Node*         l; // leaf reached
        uintptr_t     pupdate;   // p->update observed 
        uintptr_t     gpupdate;  // gp->update observed
    };

    SearchRes search(int key) const {
        InternalNode* gp = nullptr;
        InternalNode* p  = nullptr;
        uintptr_t gpupdate = 0, pupdate = 0;
        Node* l = root;
        while (!l->isLeaf) {
            gp = p;
            gpupdate = pupdate;
            p = static_cast<InternalNode*>(l);
            pupdate = p->update.load(std::memory_order_seq_cst);
            l = (key < p->key)
                ? p->left .load(std::memory_order_seq_cst)
                : p->right.load(std::memory_order_seq_cst);
        }
        return {gp, p, l, pupdate, gpupdate};
    }

    static void casChild(InternalNode* parent, Node* oldChild, Node* newChild) {
        Node* expL = oldChild;
        if (parent->left.compare_exchange_strong(expL, newChild,
                                                 std::memory_order_seq_cst))
            return;
        Node* expR = oldChild;
        parent->right.compare_exchange_strong(expR, newChild,
                                              std::memory_order_seq_cst);
    }

    void helpInsert(InsertInfo* op) {
        casChild(op->p, op->l, op->newInternal);
        uintptr_t expect = pack(op, IFLAG);
        op->p->update.compare_exchange_strong(expect, CLEAN,
                                              std::memory_order_seq_cst);
    }

    void helpMarked(DeleteInfo* op) {
        Node* other = (op->p->left.load(std::memory_order_seq_cst) == op->l)
                      ? op->p->right.load(std::memory_order_seq_cst)
                      : op->p->left .load(std::memory_order_seq_cst);
        casChild(op->gp, op->p, other);
        uintptr_t expect = pack(op, DFLAG);
        op->gp->update.compare_exchange_strong(expect, CLEAN,
                                               std::memory_order_seq_cst);
    }

    // Returns true if the delete is guaranteed to complete 
    bool helpDelete(DeleteInfo* op) {
        uintptr_t expected = op->pupdate;
        uintptr_t marked   = pack(op, MARK);
        bool ok = op->p->update.compare_exchange_strong(
            expected, marked, std::memory_order_seq_cst);
        if (ok || (getPtr(expected) == op && getState(expected) == MARK)) {
            helpMarked(op);
            return true;
        }
        help(expected);
        uintptr_t dflagged = pack(op, DFLAG);
        op->gp->update.compare_exchange_strong(dflagged, CLEAN,
                                               std::memory_order_seq_cst);
        return false;
    }

    void help(uintptr_t u) {
        Info* info = getPtr(u);
        switch (getState(u)) {
            case IFLAG: helpInsert(static_cast<InsertInfo*>(info)); break;
            case MARK : helpMarked(static_cast<DeleteInfo*>(info)); break;
            case DFLAG: helpDelete(static_cast<DeleteInfo*>(info)); break;
            default: break;
        }
    }

public:
    LockFreeBST() {
        Leaf* inf1 = new Leaf(INF1);
        Leaf* inf2 = new Leaf(INF2);
        root = new InternalNode(INF2, inf1, inf2);
    }

    bool contains(int key) const {
        SearchRes r = search(key);
        return r.l->key == key && key < INF1;
    }

    bool containsWork(int key, int workIters) const {
        SearchRes r = search(key);
        bool found = r.l->key == key && key < INF1;
        if (found) simulateWork(workIters);
        return found;
    }

    bool insert(int key) {
        while (true) {
            SearchRes r = search(key);
            if (r.l->key == key) return false;
            if (getState(r.pupdate) != CLEAN) {
                help(r.pupdate);
                continue;
            }
            Leaf* newLeaf      = new Leaf(key);
            Leaf* oldLeafClone = new Leaf(r.l->key);
            InternalNode* newInt;
            if (key < r.l->key)
                newInt = new InternalNode(r.l->key, newLeaf, oldLeafClone);
            else
                newInt = new InternalNode(key,      oldLeafClone, newLeaf);

            InsertInfo* op = new InsertInfo();
            op->p = r.p; op->newInternal = newInt; op->l = r.l;

            uintptr_t expected = r.pupdate;
            uintptr_t flagged  = pack(op, IFLAG);
            if (r.p->update.compare_exchange_strong(
                    expected, flagged, std::memory_order_seq_cst)) {
                helpInsert(op);
                return true;
            }
            help(expected);
        }
    }

    bool remove(int key) {
        while (true) {
            SearchRes r = search(key);
            if (r.l->key != key) return false;
            if (r.gp == nullptr)  return false;
            if (getState(r.gpupdate) != CLEAN) { help(r.gpupdate); continue; }
            if (getState(r.pupdate)  != CLEAN) { help(r.pupdate);  continue; }

            DeleteInfo* op = new DeleteInfo();
            op->gp = r.gp; op->p = r.p; op->l = r.l;
            op->pupdate = r.pupdate;

            uintptr_t expected = r.gpupdate;
            uintptr_t flagged  = pack(op, DFLAG);
            if (r.gp->update.compare_exchange_strong(
                    expected, flagged, std::memory_order_seq_cst)) {
                if (helpDelete(op)) return true;
                // helpDelete already cleaned gp. Retry
            } else {
                help(expected);
            }
        }
    }
};

namespace test {

void singleThreadedSanity() {
    LockFreeBST t;
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
    constexpr int  THREADS    = 8;
    constexpr int  OPS        = 20000;
    constexpr int  KEY_SPACE  = 2000;

    LockFreeBST tree;

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
            std::uniform_int_distribution<int> opD(0, 5);
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
              << "inserts="  << ins.load()
              << " removes=" << del.load()
              << " hits="    << found.load()
              << " miss="    << miss.load()
              << " final size=" << ref.size() << "\n";
}

} // namespace test

void runBenchmark(int threads, int opsPerThread, int keySpace, int workIters) {
    LockFreeBST tree;
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
            std::uniform_int_distribution<int> opD(0, 5);
            sync.arrive_and_wait();
            for (int i = 0; i < opsPerThread; ++i) {
                int k = keyD(rng);
                switch (opD(rng)) {
                    case 0: ins  += tree.insert(k);                 break;
                    case 1: del  += tree.remove(k);                 break;
                    default:
                        if (tree.containsWork(k, workIters)) ++found; else ++miss; break;
                }
            }
        });
    }
    for (auto& t : ts) t.join();

    auto t1 = std::chrono::steady_clock::now();
    long totalOps = (long)threads * opsPerThread;
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double throughput = totalOps / (us / 1e6);

    std::cout << "variant=lockFree"
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
        int workIters   = argc >= 6 ? std::atoi(argv[5]) : 0;
        runBenchmark(threads, opsPerThread, keySpace, workIters);
    } else {
        test::singleThreadedSanity();
        test::concurrentStress();
    }
    return 0;
}
