#pragma once

#include "MipmapProfile.h"

// Paused, active, active, paused. Placing the two reference blocks at
// opposite ends reduces bias from gradual changes in scene load.
class MipmapComparison
{
public:
    using Clock = MipmapProfile::Clock;
    static constexpr size_t BlockFrames = 180;
    static constexpr size_t SettleFrames = 12;

    struct Result
    {
        uint64_t intervalUs = 0, draws = 0;
        size_t frames = 0;
        double fps() const { return intervalUs ? frames * 1000000.0 / intervalUs : 0.0; }
    };

    bool active() const { return m_active; }
    bool done() const { return m_block == Schedule.size(); }
    bool paused() const { return !done() && Schedule[m_block]; }
    bool was_paused() const { return m_wasPaused; }
    size_t ready() const { return m_ready; }
    const Result& result(bool paused) const { return m_results[paused ? 0 : 1]; }

    void start(Clock::time_point now, bool wasPaused, unsigned zone, size_t ready)
    {
        m_active = true;
        m_wasPaused = wasPaused;
        m_zone = zone;
        m_ready = ready;
        m_last = now;
        m_count = m_block = m_inBlock = 0;
        m_settle = SettleFrames;
        m_results = {};
    }

    void present(Clock::time_point now, const MipmapFrameWork& work)
    {
        if (!m_active || done()) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last).count();
        m_last = now;
        if (elapsed <= 0) return;
        if (m_settle) {
            --m_settle;
            return;
        }
        const auto us = static_cast<uint64_t>(elapsed);
        m_rows[m_count++] = {m_block + 1, us, work.presentUs, work.draws, work.firstDraws};
        auto& result = m_results[paused() ? 0 : 1];
        result.intervalUs += us;
        result.draws += work.draws;
        ++result.frames;
        if (++m_inBlock == BlockFrames) {
            ++m_block;
            m_inBlock = 0;
            m_settle = SettleFrames;
        }
    }

    void reset() { m_active = false; }

    std::string save(const std::filesystem::path& install, const char* reason) const
    {
        if (m_count == 0) return {};
        const auto path = mip_log_path(install, "VanaView-mips-ab");
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wbx") != 0 || !file) return "Mipmap comparison write failed.";
        bool ok = std::fprintf(file, "# VanaView mipmap comparison schema=1 end=%s zone=%u ready=%zu rows=%zu\n",
                               reason, m_zone, m_ready, m_count) >= 0;
        ok = ok && std::fputs("phase,mode,frame,interval_us,present_us,color_draws,first_draws\n", file) >= 0;
        for (size_t i = 0; ok && i < m_count; ++i) {
            const auto& row = m_rows[i];
            ok = std::fprintf(file, "%zu,%s,%zu,%llu,%llu,%u,%u\n", row.phase,
                              Schedule[row.phase - 1] ? "paused" : "auto", i + 1, row.intervalUs,
                              row.presentUs, row.draws, row.firstDraws) >= 0;
        }
        ok = std::fclose(file) == 0 && ok;
        return ok ? "Mipmap comparison saved: " + path.string() : "Mipmap comparison write failed.";
    }

private:
    static constexpr std::array Schedule{true, false, false, true};
    struct Row
    {
        size_t phase;
        uint64_t intervalUs, presentUs;
        unsigned draws, firstDraws;
    };

    std::array<Row, Schedule.size() * BlockFrames> m_rows{};
    std::array<Result, 2> m_results{};
    bool m_active = false, m_wasPaused = false;
    unsigned m_zone = 0;
    size_t m_ready = 0, m_count = 0, m_block = 0, m_inBlock = 0, m_settle = 0;
    Clock::time_point m_last{};
};
