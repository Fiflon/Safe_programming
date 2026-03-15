// g++ main.cpp -o run && ./run N M
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

std::mutex L;
std::vector<int> acct;
std::vector<std::unique_ptr<std::mutex>> Lfine;

void move_broad(int n, int i, int j) {
    L.lock();
    acct[i] -= n;
    std::cout << n << " ... in transfer \n";
    acct[j] += n;
    L.unlock();
}
void move_finegrained_unsafe(int n, int i, int j) {
    Lfine[i]->lock();
    Lfine[j]->lock();
    acct[i] -= n;
    std::cout << n << " ... in transfer \n";
    acct[j] += n;
    Lfine[i]->lock();
    Lfine[j]->lock();
}

void move_finegrained_safe(int n, int i, int j) {
    int mi = std::min(i, j);
    int ma = std::max(i, j);
    Lfine[mi]->lock();
    Lfine[ma]->lock();
    acct[i] -= n;
    std::cout << n << " ... in transfer \n";
    acct[j] += n;
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
    if (argc < 3 ) {
        std::cout << "Usage: program [N threads] [M accounts]\n";
        return 0;
    }

    acct = std::vector<int>(m, 0);
    Lfine = std::vector<std::unique_ptr<std::mutex>>(m);

    for (auto& l : Lfine) {
        l = std::make_unique<std::mutex>();
    }
    std::vector<std::unique_ptr<std::thread> > threads;
    for (int i = 0; i < n; i++) {
        threads.emplace_back(std::make_unique<std::thread>(
            move_couple,
            i,
            100,
            m,
            move_finegrained_safe
        ));
    }
    for (auto& t : threads) {
        t->join();
    }
    int sum = std::reduce(std::begin(acct), std::end(acct));
    assert(sum == 0);
    return 0;
}
