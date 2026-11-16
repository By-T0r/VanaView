#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

struct MipmapFrameWork
{
    uint64_t readUs = 0, buildUs = 0, createUs = 0, copyUs = 0;
    uint64_t firstDrawUs = 0, presentUs = 0;
    unsigned draws = 0, firstDraws = 0;
    size_t ready = 0;
    bool pending = false, paced = false;
};

inline std::filesystem::path mip_log_path(const std::filesystem::path& install, const char* prefix)
{
    const auto dir = install / "logs";
    std::filesystem::create_directories(dir);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char name[128]{};
    std::snprintf(name, sizeof(name), "%s-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%llu.csv",
                  prefix, unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour),
                  unsigned(now.wMinute), unsigned(now.wSecond), unsigned(now.wMilliseconds),
                  GetCurrentProcessId(), GetTickCount64());
    return dir / name;
}

// Buffer an off/on comparison in memory. File I/O runs after the capture.
class MipmapProfile
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t BaselineLimit = 300;
    static constexpr size_t ActiveLimit = 1800;

    bool active() const { return m_active; }

    void start()
    {
        m_active = true;
        m_count = 0;
        if (m_last == Clock::time_point{}) m_last = Clock::now();
    }

    void present(Clock::time_point now, unsigned zone, bool off, const MipmapFrameWork& work)
    {
        if (m_last != Clock::time_point{}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last).count();
            if (elapsed > 0 && m_active && m_count < ActiveLimit) {
                m_samples[m_count++] = {static_cast<uint64_t>(elapsed), zone, work};
            } else if (elapsed > 0 && !m_active && off) {
                m_baseline[m_nextBaseline] = {static_cast<uint64_t>(elapsed), zone};
                m_nextBaseline = (m_nextBaseline + 1) % BaselineLimit;
                if (m_baselineCount < BaselineLimit) ++m_baselineCount;
            }
        }
        m_last = now;
    }

    void reset()
    {
        m_last = {};
        m_nextBaseline = m_baselineCount = m_count = 0;
        m_active = false;
    }

    std::string save(const std::filesystem::path& install, const char* reason)
    {
        m_active = false;
        if (m_count == 0) {
            reset();
            return {};
        }
        const auto path = mip_log_path(install, "VanaView-mips");
        const bool ok = write(path, reason);
        reset();
        return ok ? "Mipmap profile saved: " + path.string() : "Mipmap profile write failed.";
    }

private:
    struct Baseline { uint64_t intervalUs = 0; unsigned zone = 0; };
    struct Sample { uint64_t intervalUs = 0; unsigned zone = 0; MipmapFrameWork work; };

    bool write(const std::filesystem::path& path, const char* reason) const
    {
        // Exclusive create preserves earlier captures even on a name collision.
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wbx") != 0 || !file) return false;
        bool ok = std::fprintf(file, "# VanaView mipmap profile schema=1 end=%s baseline=%zu auto=%zu capped=%u\n",
                               reason, m_baselineCount, m_count, unsigned(m_count == ActiveLimit)) >= 0;
        ok = ok && std::fputs("mode,frame,zone,interval_us,present_us,read_us,build_thread_us,create_us,copy_us,"
                            "first_draw_us,first_draws,color_draws,ready,pending,paced\n", file) >= 0;
        const size_t first = (m_nextBaseline + BaselineLimit - m_baselineCount) % BaselineLimit;
        for (size_t i = 0; ok && i < m_baselineCount; ++i) {
            const auto& row = m_baseline[(first + i) % BaselineLimit];
            ok = std::fprintf(file, "off,%zu,%u,%llu,0,0,0,0,0,0,0,0,0,0,0\n", i + 1, row.zone, row.intervalUs) >= 0;
        }
        for (size_t i = 0; ok && i < m_count; ++i) {
            const auto& row = m_samples[i];
            const auto& w = row.work;
            ok = std::fprintf(file, "auto,%zu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%zu,%u,%u\n",
                              i + 1, row.zone, row.intervalUs, w.presentUs, w.readUs, w.buildUs, w.createUs,
                              w.copyUs, w.firstDrawUs, w.firstDraws, w.draws, w.ready, unsigned(w.pending),
                              unsigned(w.paced)) >= 0;
        }
        return std::fclose(file) == 0 && ok;
    }

    std::array<Baseline, BaselineLimit> m_baseline{};
    std::array<Sample, ActiveLimit> m_samples{};
    size_t m_nextBaseline = 0, m_baselineCount = 0, m_count = 0;
    bool m_active = false;
    Clock::time_point m_last{};
};
