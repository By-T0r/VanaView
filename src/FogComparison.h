#pragma once

#include "MipmapProfile.h"
#include "FogDeviceState.h"
#include <algorithm>
#include <functional>
#include <utility>

class FogComparison
{
public:
    using Clock = MipmapProfile::Clock;
    static constexpr size_t BlockFrames = 120;
    static constexpr size_t SettleFrames = 12;
    static constexpr auto Timeout = std::chrono::seconds(60);

    struct Settings
    {
        unsigned zone, lod, fog;
        float distance, lodScale;
        bool operator==(const Settings&) const = default;
    };

    struct Result
    {
        uint64_t totalUs = 0;
        uint64_t renderWrites = 0, shaderWrites = 0;
        size_t frames = 0;
        double fps() const { return totalUs ? frames * 1000000.0 / totalUs : 0.0; }
        double writes_per_frame() const { return frames ? double(renderWrites + shaderWrites) / frames : 0.0; }
    };

    bool active() const { return m_active; }
    bool done() const { return m_block == Schedule.size(); }
    bool optimized() const { return !active() || done() || Schedule[m_block]; }
    bool expired(Clock::time_point now) const { return now - m_started >= Timeout; }
    bool matches(const Settings& settings) const { return settings == m_settings; }
    const Result& result(bool optimized) const { return m_results[optimized]; }

    void start(Clock::time_point now, Settings settings)
    {
        m_active = true;
        m_settings = settings;
        m_started = m_last = now;
        m_count = m_block = m_inBlock = 0;
        m_settle = SettleFrames;
        m_results = {};
        m_frame = {};
    }

    void draw(unsigned primitives, FogDeviceState::Writes writes)
    {
        if (!m_active) return;
        ++m_frame.draws;
        m_frame.primitives += primitives;
        m_frame.renderWrites += writes.renderStates;
        m_frame.shaderWrites += writes.shaderConstants;
    }

    void present(Clock::time_point now)
    {
        if (!m_active || done()) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last).count();
        m_last = now;
        const auto frame = std::exchange(m_frame, {});
        if (elapsed <= 0) return;
        if (m_settle) {
            --m_settle;
            return;
        }
        const auto us = static_cast<uint64_t>(elapsed);
        m_rows[m_count++] = {m_block, us, frame};
        auto& result = m_results[Schedule[m_block]];
        result.totalUs += us;
        result.renderWrites += frame.renderWrites;
        result.shaderWrites += frame.shaderWrites;
        ++result.frames;
        if (++m_inBlock == BlockFrames) {
            ++m_block;
            m_inBlock = 0;
            m_settle = SettleFrames;
        }
    }

    void reset() { m_active = false; }

    double low_fps(bool optimized) const
    {
        // Invert the average interval of the slowest 1% of the measured frames.
        std::array<uint64_t, Schedule.size() * BlockFrames> times{};
        size_t count = 0;
        for (size_t i = 0; i < m_count; ++i)
            if (Schedule[m_rows[i].block] == optimized) times[count++] = m_rows[i].intervalUs;
        if (!count) return 0.0;
        const size_t tail = (count + 99) / 100;
        std::partial_sort(times.begin(), times.begin() + tail, times.begin() + count, std::greater<>{});
        uint64_t total = 0;
        for (size_t i = 0; i < tail; ++i) total += times[i];
        return total ? tail * 1000000.0 / total : 0.0;
    }

    std::string save(const std::filesystem::path& install, const char* reason) const
    {
        if (m_count == 0) return {};
        const auto path = mip_log_path(install, "VanaView-fog-ab");
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wbx") != 0 || !file) return "Fog comparison write failed.";
        bool ok = std::fprintf(file,
            "# VanaView fog comparison schema=2 end=%s zone=%u lod=%u lod_scale=%.3f fog=%u distance=%.6f rows=%zu\n",
            reason, m_settings.zone, m_settings.lod, m_settings.lodScale, m_settings.fog, m_settings.distance, m_count) >= 0;
        ok = ok && std::fputs("block,implementation,frame,interval_us,draws,primitives,render_state_writes,shader_constant_writes\n", file) >= 0;
        for (size_t i = 0; ok && i < m_count; ++i) {
            const auto& row = m_rows[i];
            ok = std::fprintf(file, "%zu,%s,%zu,%llu,%llu,%llu,%llu,%llu\n", row.block + 1,
                Schedule[row.block] ? "optimized" : "legacy", i + 1, row.intervalUs,
                row.frame.draws, row.frame.primitives, row.frame.renderWrites, row.frame.shaderWrites) >= 0;
        }
        ok = std::fclose(file) == 0 && ok;
        return ok ? "Fog comparison saved: " + path.string() : "Fog comparison write failed.";
    }

private:
    static constexpr std::array Schedule{false, true, true, false};
    struct FrameWork
    {
        uint64_t draws = 0, primitives = 0, renderWrites = 0, shaderWrites = 0;
    };
    struct Row
    {
        size_t block;
        uint64_t intervalUs;
        FrameWork frame;
    };

    bool m_active = false;
    size_t m_count = 0, m_block = 0, m_inBlock = 0, m_settle = 0;
    Settings m_settings{};
    Clock::time_point m_started{}, m_last{};
    FrameWork m_frame{};
    std::array<Result, 2> m_results{};
    std::array<Row, Schedule.size() * BlockFrames> m_rows{};
};
