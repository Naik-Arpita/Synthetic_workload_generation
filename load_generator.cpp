#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <atomic>
#include <numeric>
#include <iomanip>
#include <sstream>

#include "json.hpp"
using json = nlohmann::json;

#if defined(_WIN32)
    #include <windows.h>
    #include <timeapi.h>
    #pragma comment(lib, "winmm.lib")
#elif defined(__linux__)
    #include <pthread.h>
    #include <time.h>
    #include <unistd.h>
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

struct TraceSnapshot {
    int snapshot_index;
    std::string timestamp_iso;
    double timestamp_sec;
    double total_cpu_utilization;
    int active_cores_A;
    std::vector<double> per_core_utilization;
};

struct TraceData {
    int total_cores_A = 8;
    double interval_sec = 2.0;
    std::string host_tag = "Unknown";
    std::string os_tag = "Unknown";
    std::vector<TraceSnapshot> snapshots;
};

TraceData parse_njmon_json_nlohmann(const std::string& filepath) {
    TraceData trace;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[FATAL ERROR] Unable to open JSON file: " << filepath << std::endl;
        return trace;
    }

    std::cout << "[STEP 1: PARSING] Reading and parsing JSON using nlohmann::json..." << std::endl;

    std::vector<json> raw_snapshots;
    
    try {
        json j = json::parse(file);
        if (j.is_array()) {
            for (auto& elem : j) raw_snapshots.push_back(elem);
        } else if (j.is_object()) {
            raw_snapshots.push_back(j);
        }
    } catch (...) {
        file.clear();
        file.seekg(0, std::ios::beg);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            try {
                json j = json::parse(line);
                raw_snapshots.push_back(j);
            } catch (...) {
                // Skip invalid lines
            }
        }
    }

    std::cout << "[STEP 1: PARSING] Processed " << raw_snapshots.size() << " raw JSON entries." << std::endl;

    int snap_idx = 0;
    double accum_time = 0.0;

    for (const auto& j : raw_snapshots) {
        if (trace.host_tag == "Unknown" && j.contains("tags") && j["tags"].contains("host_tag")) {
            trace.host_tag = j["tags"]["host_tag"].get<std::string>();
        }
        if (j.contains("tags") && j["tags"].contains("os_tag")) {
            trace.os_tag = j["tags"]["os_tag"].get<std::string>();
        }

        if (j.contains("lscpu") && j["lscpu"].contains("cpus_num")) {
            trace.total_cores_A = j["lscpu"]["cpus_num"].get<int>();
        } else if (j.contains("cpu_total") && j["cpu_total"].contains("cpus")) {
            trace.total_cores_A = j["cpu_total"]["cpus"].get<int>();
        }

        if (j.contains("cpu_total") && j["cpu_total"].contains("idle")) {
            TraceSnapshot snap;
            snap.snapshot_index = snap_idx + 1;
            snap.timestamp_sec = accum_time;

            if (j.contains("timestamp") && j["timestamp"].contains("datetime")) {
                snap.timestamp_iso = j["timestamp"]["datetime"].get<std::string>();
            }

            double total_idle = j["cpu_total"]["idle"].get<double>();
            snap.total_cpu_utilization = std::max(0.0, std::min(100.0, 100.0 - total_idle));

            int active_count = 0;
            if (j.contains("cpus") && j["cpus"].is_object()) {
                for (auto& [core_key, core_data] : j["cpus"].items()) {
                    if (core_data.contains("idle")) {
                        double c_idle = core_data["idle"].get<double>();
                        double c_util = 100.0 - c_idle;
                        snap.per_core_utilization.push_back(c_util);
                        if (c_util > 1.0) active_count++;
                    }
                }
            }

            snap.active_cores_A = (active_count > 0) ? active_count : 
                std::max(1, static_cast<int>(std::round((snap.total_cpu_utilization / 100.0) * trace.total_cores_A)));

            trace.snapshots.push_back(snap);
            snap_idx++;
            accum_time += 2.0;
        }
    }

    std::cout << "[STEP 1 COMPLETE] Metadata extracted:" << std::endl;
    std::cout << "  - Host          : " << trace.host_tag << std::endl;
    std::cout << "  - OS            : " << trace.os_tag << std::endl;
    std::cout << "  - Platform A Cores : " << trace.total_cores_A << std::endl;
    std::cout << "  - Valid Snapshots  : " << trace.snapshots.size() << "\n" << std::endl;

    return trace;
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

    const double slice_sec = 0.010; 
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

        
        if (target_spin_sec > 0.0) {
            while (true) {
                auto now = std::chrono::steady_clock::now();
                double elapsed_spin = std::chrono::duration<double>(now - spin_start_wall).count();
                if (elapsed_spin >= target_spin_sec) break;
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

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif

    std::string trace_file = (argc > 1) ? argv[1] : "5CG04131YG_20260727_0857.json";

    std::cout << "========================================================================\n";
    std::cout << "        SYNTHETIC CPU LOAD GENERATOR (METRIC-LEVEL REPRODUCTION)        \n";
    std::cout << "========================================================================\n";
    std::cout << "[INIT] Targeted trace file: " << trace_file << std::endl;

    TraceData trace = parse_njmon_json_nlohmann(trace_file);
    if (trace.snapshots.empty()) {
        std::cerr << "[FATAL] No valid snapshots found. Program exiting." << std::endl;
        return 1;
    }

    int N_A = trace.total_cores_A;
    int N_B = std::thread::hardware_concurrency();

    
    std::cout << "[STEP 2: TRANSLATION MATH] Calculating active core fractions..." << std::endl;
    double total_active_fraction = 0.0;
    for (const auto& s : trace.snapshots) {
        total_active_fraction += (static_cast<double>(s.active_cores_A) / N_A);
    }
    double avg_active_fraction = total_active_fraction / trace.snapshots.size();
    int N_workers_B = std::max(1, static_cast<int>(std::round(avg_active_fraction * N_B)));

    std::cout << "  - Platform A Total Cores (N_A)     : " << N_A << std::endl;
    std::cout << "  - Platform B Total Cores (N_B)     : " << N_B << std::endl;
    std::cout << "  - Active Core Fraction             : " << std::fixed << std::setprecision(2) << (avg_active_fraction * 100.0) << "%" << std::endl;
    std::cout << "  - Target Workers on Platform B     : " << N_workers_B << "\n" << std::endl;

    
    std::cout << "[STEP 3: WORKER INITIALIZATION] Launching " << N_workers_B << " worker threads..." << std::endl;
    std::vector<std::thread> workers;
    for (int i = 0; i < N_workers_B; ++i) {
        workers.emplace_back(worker_thread_func, i); 
    }

    
    std::cout << "\n========================================================================" << std::endl;
    std::cout << "  REPLAYING TRACE SNAPSHOT-BY-SNAPSHOT (10ms SLICE WITH FEEDBACK)       " << std::endl;
    std::cout << "========================================================================" << std::endl;

    double engine_start_wall = get_wall_time_sec();

    for (size_t idx = 0; idx < trace.snapshots.size(); ++idx) {
        const auto& snap = trace.snapshots[idx];
        g_current_target_load.store(snap.total_cpu_utilization);

        double elapsed_sec = get_wall_time_sec() - engine_start_wall;

        std::cout << "[REPLAY] Snap " << std::setw(4) << snap.snapshot_index << "/" << trace.snapshots.size()
                  << " | Time: " << std::setw(6) << std::fixed << std::setprecision(1) << elapsed_sec << "s"
                  << " | ISO: " << (snap.timestamp_iso.empty() ? "N/A" : snap.timestamp_iso)
                  << " | Target CPU: " << std::setw(6) << std::fixed << std::setprecision(2) << snap.total_cpu_utilization << "%"
                  << " | Active Cores (A): " << snap.active_cores_A
                  << " | Workers (B): " << N_workers_B << std::endl;

        std::this_thread::sleep_for(std::chrono::duration<double>(trace.interval_sec));
    }

    
    std::cout << "\n[STEP 4: CLEANUP] Playback finished. Stopping workers..." << std::endl;
    g_running.store(false);

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

#if defined(_WIN32)
    timeEndPeriod(1);
#endif

    std::cout << "[SUCCESS] Synthetic workload generation finished successfully." << std::endl;
    return 0;
}