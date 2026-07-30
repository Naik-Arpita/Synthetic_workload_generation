
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <iomanip>

#if defined(__linux__)
    #include <pthread.h>
    #include <time.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <timeapi.h>
    #pragma comment(lib, "winmm.lib")
#endif


double get_wall_time_sec() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

double get_thread_cpu_time_sec() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#elif defined(_WIN32)
    FILETIME ftCreation, ftExit, ftKernel, ftUser;
    if (GetThreadTimes(GetCurrentThread(), &ftCreation, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER k, u;
        k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
        u.LowPart = ftUser.dwLowDateTime; u.HighPart = ftUser.dwHighDateTime;
        return (k.QuadPart + u.QuadPart) * 1e-7;
    }
    return 0.0;
#else
    return 0.0;
#endif
}

std::atomic<bool> g_running{true};
std::atomic<double> g_current_target_load{0.0};

void worker_thread_func(int thread_id) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id % std::thread::hardware_concurrency(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    const double slice_sec = 0.010; // 10 ms slice
    double sleep_correction = 0.0;

    while (g_running.load()) {
        double target_pct = g_current_target_load.load();
        if (target_pct <= 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        double spin_ratio = target_pct / 100.0;
        double target_spin_sec = slice_sec * spin_ratio;
        double target_sleep_sec = slice_sec * (1.0 - spin_ratio);

        double start_cpu = get_thread_cpu_time_sec();
        auto spin_start_wall = std::chrono::steady_clock::now();

        // 1. Direct Time-Based Spin Phase
        if (target_spin_sec > 0.0) {
            while (true) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - spin_start_wall).count() >= target_spin_sec) break;
            }
        }

        double adjusted_sleep_sec = target_sleep_sec - sleep_correction;
        if (adjusted_sleep_sec > 0.0002) {
#if defined(_WIN32)
            Sleep(static_cast<DWORD>(adjusted_sleep_sec * 1000.0));
#else
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = static_cast<long>(adjusted_sleep_sec * 1e9);
            nanosleep(&req, NULL);
#endif
        }

      
        double end_cpu = get_thread_cpu_time_sec();
        double actual_cpu_used = end_cpu - start_cpu;
        double error = actual_cpu_used - target_spin_sec;

        sleep_correction += error * 0.1;
        sleep_correction = std::max(-0.005, std::min(0.005, sleep_correction));
    }
}

int main() {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif

    size_t num_snapshots = 0;
    int N_A = 8;
    double avg_active_fraction = 0.0;

    // Read pre-parsed header metrics from Python pipe
    if (!(std::cin >> num_snapshots >> N_A >> avg_active_fraction)) {
        std::cerr << "[FATAL] Failed to read stream header from Python parser." << std::endl;
        return 1;
    }

    std::vector<double> target_loads(num_snapshots);
    for (size_t i = 0; i < num_snapshots; ++i) {
        std::cin >> target_loads[i];
    }

    int N_B = std::thread::hardware_concurrency();
    int N_workers_B = std::max(1, static_cast<int>(std::round(avg_active_fraction * N_B)));

    std::cout << "========================================================================\n";
    std::cout << "        HYBRID WORKLOAD ENGINE (PYTHON PARSER + C++ TIMING ENGINE)       \n";
    std::cout << "========================================================================\n";
    std::cout << "[INIT] Loaded " << num_snapshots << " snapshots from Python stream.\n";
    std::cout << "  - Platform A Cores (N_A)     : " << N_A << std::endl;
    std::cout << "  - Platform B Cores (N_B)     : " << N_B << std::endl;
    std::cout << "  - Target Workers on Platform B : " << N_workers_B << "\n" << std::endl;

    // Launch C++ Worker Threads
    std::vector<std::thread> workers;
    for (int i = 0; i < N_workers_B; ++i) {
        workers.emplace_back(worker_thread_func, i);
    }

    
    double engine_start_wall = get_wall_time_sec();

    for (size_t idx = 0; idx < target_loads.size(); ++idx) {
        double util = target_loads[idx];
        g_current_target_load.store(util);

        double elapsed_sec = get_wall_time_sec() - engine_start_wall;

        std::cout << "[REPLAY] Snap " << std::setw(4) << (idx + 1) << "/" << num_snapshots
                  << " | Elapsed: " << std::setw(6) << std::fixed << std::setprecision(1) << elapsed_sec << "s"
                  << " | Target CPU: " << std::setw(6) << std::fixed << std::setprecision(2) << util << "%"
                  << " | Workers (B): " << N_workers_B << std::endl;

        std::this_thread::sleep_for(std::chrono::duration<double>(2.0)); // 2s sampling interval
    }

    
    g_running.store(false);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

#if defined(_WIN32)
    timeEndPeriod(1);
#endif

    std::cout << "[SUCCESS] Synthetic workload generation complete." << std::endl;
    return 0;
}