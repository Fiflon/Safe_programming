// g++ main.cpp -o run && ./run N M K
// 1. ma sie zablokowac (unsafe)
// 2. wykresy finegrained vs borad - cpu do throughput(X/{czas na X operacji})
// 2. wykresy finegrained vs borad - cpu do latency
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

std::mutex L;
std::vector<int> acct;
std::vector<std::unique_ptr<std::mutex>> Lfine;

#define SLEEP true

struct Timer {
    std::chrono::high_resolution_clock::time_point start;

    std::string msg;
    Timer(std::string msg = "") {
        this->msg = msg;
        start = std::chrono::high_resolution_clock::now();
    }

    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << msg << "Execution time: " << duration.count() << " ms\n";
    }
};

void random_sleep(int min_ms, int max_ms) {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(min_ms, max_ms);

    int duration = dist(gen);
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
}


void work(int n, int i, int j) {
    acct[i] -= n;
    std::cerr << n << " ... in transfer from \t" << i << " -> " << j << "\n";
    acct[j] += n;
    if (SLEEP) {
        random_sleep(1, 10); 
    }
}

void move_broad(int n, int i, int j) {
    L.lock();
    work(n, i, j);
    L.unlock();
}

void move_finegrained_unsafe(int n, int i, int j) {
    Lfine[i]->lock();
    Lfine[j]->lock();
    work(n, i, j);
    Lfine[i]->unlock();
    Lfine[j]->unlock();
}

void move_finegrained_safe(int n, int i, int j) {
    int mi = std::min(i, j);
    int ma = std::max(i, j);
    Lfine[mi]->lock();
    Lfine[ma]->lock();
    work(n, i, j);
    Lfine[ma]->unlock();
    Lfine[mi]->unlock();
}

void move_couple(int id, int transfers_count, int account_range, std::function<void(int, int, int)> move_func) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, account_range-1);
    std::uniform_int_distribution<> transferred(1, 100);

    for (int i = 0; i < transfers_count; i++) {
        int a = dist(gen);
        int b = dist(gen);
        while (b == a) {
            b = dist(gen);
        }
        int amount = transferred(gen);
        move_func(amount, a, b);
    }
}

int main(int argc, char** argv) {
    int n = std::stoi(argv[1]);
    int m = std::stoi(argv[2]);
    int k = std::stoi(argv[3]);
    std::string type = argv[4];
    if (argc < 3 ) {
        std::cout << "Usage: program [N threads] [M accounts] [K transfers]\n";
        return 0;
    }

    acct = std::vector<int>(m, 0);
    Lfine = std::vector<std::unique_ptr<std::mutex>>(m);

    std::function<void(int, int, int)> func;
    if (type == "safe") {
        func = move_finegrained_safe;
    }else if (type == "unsafe") {
        func = move_finegrained_unsafe;
    }else if (type == "broad") {
        func = move_broad;
    }else {
        std::cout << "Wrong move type\n";
        return 0;
    }
    if (k % n != 0) {
        std::cout << "SOME TRANSFERS WILL BE LOST!\n";
    }
    for (auto& l : Lfine) {
        l = std::make_unique<std::mutex>();
    }
    std::vector<std::unique_ptr<std::thread> > threads;
    {
        Timer timer("Time it took for " + type + ":\t");
        for (int i = 0; i < n; i++) {
            threads.emplace_back(std::make_unique<std::thread>(
                move_couple,
                i, //id
                k / n, //number of transfers
                m, //number of accounts
                func
            ));
        }
        for (auto& t : threads) {
            t->join();
        }
    }
    int sum = std::reduce(std::begin(acct), std::end(acct));
    assert(sum == 0);
    return 0;
}
