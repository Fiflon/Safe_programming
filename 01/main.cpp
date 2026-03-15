// g++ main.cpp -o run && ./run N M K
// 1. ma sie zablokowac (unsafe)
// 2. wykresy finegrained vs borad - cpu do throughput(X/{czas na X operacji})
// 2. wykresy finegrained vs borad - cpu do latency
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

std::mutex L;
std::vector<int> accounts;
std::vector<std::unique_ptr<std::mutex>> Lfine;
bool verbose = false;

void transfer(int n, int i, int j)
{
    accounts[i] -= n;
    accounts[j] += n;
    if (verbose)
    {
        std::cerr << n << " ... in transfer from \t" << i << " -> " << j << "\n";
    }
}

void move_broad(int n, int i, int j)
{
    L.lock();
    transfer(n, i, j);
    L.unlock();
}

void move_finegrained_unsafe(int n, int i, int j)
{
    Lfine[i]->lock();
    Lfine[j]->lock();
    transfer(n, i, j);
    Lfine[i]->unlock();
    Lfine[j]->unlock();
}

void move_finegrained_safe(int n, int i, int j)
{
    int mi = std::min(i, j);
    int ma = std::max(i, j);
    Lfine[mi]->lock();
    Lfine[ma]->lock();
    transfer(n, i, j);
    Lfine[ma]->unlock();
    Lfine[mi]->unlock();
}

void move_couple(int id, int transfers_count, int account_range, std::function<void(int, int, int)> move_func)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, account_range - 1);
    std::uniform_int_distribution<> transferred(1, 100);

    for (int i = 0; i < transfers_count; i++)
    {
        int a = dist(gen);
        int b = dist(gen);
        while (b == a)
        {
            b = dist(gen);
        }
        int amount = transferred(gen);
        move_func(amount, a, b);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::cout << "Usage: program [N threads] [M accounts] [K transfers] [safe|unsafe|broad] [verbose=0|1]\n";
        return 0;
    }

    int n = std::stoi(argv[1]);
    int m = std::stoi(argv[2]);
    int k = std::stoi(argv[3]);
    std::string type = argv[4];
    if (argc >= 6)
    {
        verbose = (std::stoi(argv[5]) != 0);
    }

    accounts = std::vector<int>(m, 0);
    Lfine = std::vector<std::unique_ptr<std::mutex>>(m);

    std::function<void(int, int, int)> func;
    if (type == "safe")
    {
        func = move_finegrained_safe;
    }
    else if (type == "unsafe")
    {
        func = move_finegrained_unsafe;
    }
    else if (type == "broad")
    {
        func = move_broad;
    }
    else
    {
        std::cout << "Wrong move type\n";
        return 0;
    }
    if (k % n != 0)
    {
        std::cout << "SOME TRANSFERS WILL BE LOST!\n";
    }
    for (auto &l : Lfine)
    {
        l = std::make_unique<std::mutex>();
    }
    std::vector<std::unique_ptr<std::thread>> threads;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < n; i++)
    {
        threads.emplace_back(std::make_unique<std::thread>(
            move_couple,
            i,     // id
            k / n, // number of transfers
            m,     // number of accounts
            func));
    }
    for (auto &t : threads)
    {
        t->join();
    }
    auto end = std::chrono::high_resolution_clock::now();

    const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    const long long executed_transfers = static_cast<long long>(k / n) * n;
    const double throughput_ops_per_sec = (duration_us > 0)
                                              ? (static_cast<double>(executed_transfers) * 1'000'000.0 / static_cast<double>(duration_us))
                                              : 0.0;
    const double latency_us_per_op = (executed_transfers > 0)
                                         ? (static_cast<double>(duration_us) / static_cast<double>(executed_transfers))
                                         : 0.0;

    std::cout << "mode=" << type
              << " threads=" << n
              << " accounts=" << m
              << " transfers=" << executed_transfers
              << " time_us=" << duration_us
              << " throughput_ops_s=" << throughput_ops_per_sec
              << " latency_us_op=" << latency_us_per_op
              << "\n";

    int sum = std::reduce(std::begin(accounts), std::end(accounts));
    assert(sum == 0);
    return 0;
}
