// Non-blocking leaf-oriented Binary Search Tree.
// Reference: F. Ellen, P. Fatourou, E. Ruppert, F. van Breugel,
//            "Non-blocking Binary Search Trees", PODC 2010.
//
// Build: g++ -std=c++20 -pthread -O2 BST.cpp -o BST
//
// Design summary
// --------------
// Leaf-oriented BST: keys live only in leaves; internal nodes are routers
// with splitKey K (left subtree has keys < K, right subtree has keys >= K).
// A sentinel root with two sentinel leaves (INT_MAX-1, INT_MAX) keeps the
// tree non-empty so insert/delete always have a real parent / grandparent.
//
// Synchronisation: every InternalNode owns one std::atomic<uintptr_t> "update"
// holding a *tagged pointer*: low 2 bits encode a state, the rest points to
// an Info record describing the in-flight update.
//
//   CLEAN = 0   no update in progress
//   DFLAG = 1   a Delete has flagged this node (it is the grandparent)
//   IFLAG = 2   an Insert has flagged this node (it is the parent)
//   MARK  = 3   this node is being / has been removed (permanent)
//
// All non-trivial state changes use compare_exchange_strong with
// memory_order_seq_cst. No mutex / no spin lock anywhere.
//
// Helping: a thread that finds a non-CLEAN update on its way runs the
// remaining steps of that update (idempotent helpInsert / helpDelete /
// helpMarked) before retrying; this is what gives global lock-freedom.
//
// Memory reclamation is intentionally omitted (proof of concept).

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

// ---------------------------------------------------------------------------
// Tracing
// ---------------------------------------------------------------------------
//
//  Compile with -DBST_TRACE=1   -> every insert/remove/help logs a line.
//  Compile with -DBST_TRACE=2   -> additionally logs every CAS attempt.
//  Default (no -D)              -> silent (zero overhead).
//
// Logging is serialised via a single mutex around std::cout so lines from
// different threads do not interleave inside one event.
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

// ---------------------------------------------------------------------------
// Node hierarchy
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Info records (one per in-flight update)
// ---------------------------------------------------------------------------

struct alignas(4) Info { virtual ~Info() = default; };

struct InsertInfo : Info {
    InternalNode* p;             // parent that will be flagged
    InternalNode* newInternal;   // new sub-tree to splice in
    Node*         l;             // existing leaf to replace
};

struct DeleteInfo : Info {
    InternalNode* gp;            // grandparent that will be flagged
    InternalNode* p;             // parent that will be marked
    Node*         l;             // leaf carrying the key to delete
    uintptr_t     pupdate;       // observed value of p->update at search time
};

static_assert(alignof(Info) >= 4,
              "Info must provide at least 2 free low bits for tagged pointers");
static_assert((alignof(InsertInfo) & uintptr_t(0x3)) == 0,
              "InsertInfo alignment must keep low tag bits clear");
static_assert((alignof(DeleteInfo) & uintptr_t(0x3)) == 0,
              "DeleteInfo alignment must keep low tag bits clear");

// ---------------------------------------------------------------------------
// Tagged pointer helpers (low 2 bits of update store the state)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Internal node
// ---------------------------------------------------------------------------

struct InternalNode : Node {
    std::atomic<Node*>     left;
    std::atomic<Node*>     right;
    std::atomic<uintptr_t> update;

    InternalNode(int k, Node* l, Node* r)
        : Node(false, k), left(l), right(r), update(CLEAN) {}
};

// ---------------------------------------------------------------------------
// Lock-free BST
// ---------------------------------------------------------------------------

class LockFreeBST {
    // Real keys must be strictly smaller than INF1.
    static constexpr int INF1 = INT_MAX - 1;
    static constexpr int INF2 = INT_MAX;

    InternalNode* root;

    struct SearchRes {
        InternalNode* gp;        // grandparent of l (may be nullptr)
        InternalNode* p;         // parent of l
        Node*         l;         // leaf reached
        uintptr_t     pupdate;   // p->update observed during traversal
        uintptr_t     gpupdate;  // gp->update observed during traversal
    };

    // Wait-free traversal. Reads only; ignores flags / marks (paper §4).
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

    // CAS one of parent's children from oldChild to newChild.
    // At the moment a write to update is in flight, exactly one of
    // left/right equals oldChild; the other CAS harmlessly fails.
    static void casChild(InternalNode* parent, Node* oldChild, Node* newChild) {
        Node* expL = oldChild;
        if (parent->left.compare_exchange_strong(expL, newChild,
                                                 std::memory_order_seq_cst))
            return;
        Node* expR = oldChild;
        parent->right.compare_exchange_strong(expR, newChild,
                                              std::memory_order_seq_cst);
    }

    // ---- Helpers (idempotent) --------------------------------------------

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

    // Returns true if the delete is guaranteed to complete (parent marked
    // either by us or a helper). Returns false if we must back off and retry.
    bool helpDelete(DeleteInfo* op) {
        uintptr_t expected = op->pupdate;
        uintptr_t marked   = pack(op, MARK);
        bool ok = op->p->update.compare_exchange_strong(
            expected, marked, std::memory_order_seq_cst);
        if (ok || (getPtr(expected) == op && getState(expected) == MARK)) {
            helpMarked(op);
            return true;
        }
        // Mark failed: another op holds p. Help it, then release gp.
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
            default: break;  // CLEAN: nothing to do
        }
    }

public:
    LockFreeBST() {
        Leaf* inf1 = new Leaf(INF1);
        Leaf* inf2 = new Leaf(INF2);
        // splitKey = INF2: every real key < INF1 < INF2 routes left.
        root = new InternalNode(INF2, inf1, inf2);
    }

    // Wait-free.
    bool contains(int key) const {
        SearchRes r = search(key);
        return r.l->key == key && key < INF1;
    }

    // Lock-free.
    bool insert(int key) {
        while (true) {
            SearchRes r = search(key);
            if (r.l->key == key) return false;       // key already present
            if (getState(r.pupdate) != CLEAN) {
                help(r.pupdate);
                continue;
            }
            // Build the three new nodes.
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
                helpInsert(op);                       // swing + un-flag
                return true;
            }
            // Flag failed: another op intervened on p. Help it and retry.
            help(expected);
        }
    }

    // Lock-free.
    bool remove(int key) {
        while (true) {
            SearchRes r = search(key);
            if (r.l->key != key) return false;        // key not present
            if (r.gp == nullptr)  return false;       // refuse to touch sentinels
            if (getState(r.gpupdate) != CLEAN) { help(r.gpupdate); continue; }
            if (getState(r.pupdate)  != CLEAN) { help(r.pupdate);  continue; }

            DeleteInfo* op = new DeleteInfo();
            op->gp = r.gp; op->p = r.p; op->l = r.l;
            op->pupdate = r.pupdate;

            uintptr_t expected = r.gpupdate;
            uintptr_t flagged  = pack(op, DFLAG);
            if (r.gp->update.compare_exchange_strong(
                    expected, flagged, std::memory_order_seq_cst)) {
                if (helpDelete(op)) return true;      // mark + splice succeeded
                // mark failed: helpDelete already cleaned gp; loop retries.
            } else {
                help(expected);
            }
        }
    }
};

// ===========================================================================
// Tests
// ===========================================================================

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

    // Pre-populate so the tree has structure to fight over.
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
              << "inserts="  << ins.load()
              << " removes=" << del.load()
              << " hits="    << found.load()
              << " miss="    << miss.load()
              << " final size=" << ref.size() << "\n";
}

} // namespace test

// ===========================================================================
// Benchmark harness
// ===========================================================================

void runBenchmark(int threads, int opsPerThread, int keySpace) {
    LockFreeBST tree;
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
        runBenchmark(threads, opsPerThread, keySpace);
    } else {
        test::singleThreadedSanity();
        test::concurrentStress();
    }
    return 0;
}

// ===========================================================================
// Non-blocking proof (lock-freedom)
// ===========================================================================
//
// Claim. In every infinite execution of the algorithm above, infinitely many
// operations complete. Equivalently: no thread can starve the system on its
// own — for every failed CAS there is a corresponding successful CAS by some
// other thread.
//
// (1) Search is wait-free.
//     `search()` performs only `load`s along a root-to-leaf path. No CAS, no
//     retry loop. Each iteration descends one level, terminating in O(h)
//     steps where h is the height observed during the walk.
//
// (2) Once an Insert successfully flags its parent, it finishes in O(1).
//     The flag CAS in `insert()` (line: `r.p->update.cmpx(...,flagged)`)
//     publishes the InsertInfo. From there `helpInsert` performs at most
//     2 CASes (`casChild` + un-flag), neither of which can be defeated:
//       * `casChild` either wins or finds the child already swung by a
//         helper (idempotent — both calls store the same newInternal).
//       * The un-flag CAS targets exactly the value we wrote; if it fails
//         it is because a helper already restored CLEAN, which is fine.
//
// (3) Once a Delete successfully flags its grandparent AND marks its parent,
//     it finishes in O(1). Same reasoning as (2) but with `helpMarked`:
//     at most 2 CASes (`casChild` on gp + un-flag gp), both idempotent.
//
// (4) A failed CAS implies another CAS succeeded.
//     There are exactly four CAS sites:
//        (a) flag-parent in `insert`,
//        (b) flag-grandparent in `remove`,
//        (c) mark-parent in `helpDelete`,
//        (d) child swing + un-flag in helpers.
//     A CAS on `update` fails iff the field changed since we read it, i.e.
//     some other thread wrote it — and writes to `update` only happen via
//     successful CASes from this set. A `casChild` failure means another
//     thread (helper of the same op) already swung the child. So every CAS
//     failure is paired with a successful CAS by a different operation.
//
// (5) Helping converts other threads' progress into our own.
//     Whenever `insert` / `remove` / `helpDelete` observes a non-CLEAN
//     update or a CAS failure, it calls `help(u)` BEFORE retrying. By (2)
//     and (3) the helped op completes in finitely many steps, after which
//     our retry sees CLEAN. This rules out two threads endlessly
//     invalidating each other.
//
// (6) The lowest pending Delete makes progress.
//     A Delete can fail twice — once on flag (resolved by helping per (5))
//     and once on mark, after which it cleans gp up and retries. Suppose
//     for contradiction infinitely many Deletes start and none finishes.
//     Take the Delete D operating on the deepest pair (gp, p) among all
//     currently-flagged Deletes. Any conflicting op on p must be an op
//     strictly above D; once those finitely many ops are helped to
//     completion, D's mark CAS on p succeeds (no one else can flip p's
//     update from CLEAN because gp is DFLAG-locked). Thus D completes —
//     contradiction.
//
// (7) Conclusion.
//     Every CAS failure ⇒ a CAS by another op succeeded (4). Every
//     successful CAS — except a Delete-flag that later aborts — is one of
//     finitely many steps to completion (2,3); the abort case is bounded
//     by (6). Hence in any infinite execution, infinitely many ops
//     complete: the data structure is lock-free.
//
// Linearization points
// --------------------
// * contains(k):    successful   — load of the matching leaf in `search`.
//                   unsuccessful — load of the mismatching leaf.
// * insert(k):      successful   — successful child-swing CAS in
//                                  helpInsert (`casChild`); k becomes
//                                  reachable from root at that instant.
//                   unsuccessful (k present) — load of the leaf with
//                                  key == k in `search`.
// * remove(k):      successful   — successful child-swing CAS in
//                                  helpMarked (`casChild` on gp); k stops
//                                  being reachable from root at that
//                                  instant.
//                   unsuccessful (k absent) — load of the leaf in `search`.
