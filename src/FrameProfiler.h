#pragma once
#include <array>
#include <chrono>
#include <string>
#include <vector>

// ZE-125: rolling per-phase frame timing for the editor's stats overlay.
// Every metric keeps a short window and reports current / average / min / max.
namespace zengine::profiling
{
    class Metric
    {
    public:
        void Add(double value)
        {
            current_ = value;
            samples_[head_] = value;
            head_ = (head_ + 1) % samples_.size();
            if (count_ < samples_.size()) ++count_;
        }
        double Current() const { return current_; }
        double Average() const
        {
            if (!count_) return 0;
            double sum = 0;
            for (std::size_t i = 0; i < count_; ++i) sum += samples_[i];
            return sum / static_cast<double>(count_);
        }
        double Min() const
        {
            if (!count_) return 0;
            double m = samples_[0];
            for (std::size_t i = 1; i < count_; ++i) m = samples_[i] < m ? samples_[i] : m;
            return m;
        }
        double Max() const
        {
            if (!count_) return 0;
            double m = samples_[0];
            for (std::size_t i = 1; i < count_; ++i) m = samples_[i] > m ? samples_[i] : m;
            return m;
        }
        bool Empty() const { return count_ == 0; }

    private:
        std::array<double, 120> samples_{};
        std::size_t head_ = 0, count_ = 0;
        double current_ = 0;
    };

    // Named metrics, grouped for the collapsible overlay.
    struct Group
    {
        std::string title;
        std::vector<std::pair<std::string, Metric*>> rows;
    };

    class FrameProfiler
    {
    public:
        Metric fps, tps;                        // rates (Hz)
        Metric frameMs;                          // whole editor frame
        Metric renderMs, render2dMs, sceneBuildMs;
        Metric scriptMs, physicsMs, inputMs, audioMs;

        std::vector<Group> Groups()
        {
            return {
                {"Rates",       {{"Render FPS", &fps}, {"Sim TPS", &tps}}},
                {"Frame",       {{"Frame", &frameMs}, {"Scene build", &sceneBuildMs}}},
                {"Render",      {{"3D render", &renderMs}, {"2D render", &render2dMs}}},
                {"Simulation",  {{"Scripts", &scriptMs}, {"Physics", &physicsMs}, {"Input", &inputMs}, {"Audio", &audioMs}}},
            };
        }
    };

    // Adds the elapsed milliseconds to a metric when it goes out of scope.
    class ScopedTimer
    {
    public:
        explicit ScopedTimer(Metric& metric) : metric_(metric), start_(std::chrono::steady_clock::now()) {}
        ~ScopedTimer()
        {
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
            metric_.Add(ms);
        }
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

    private:
        Metric& metric_;
        std::chrono::steady_clock::time_point start_;
    };

    // Accumulates elapsed milliseconds across several calls, committed once per frame.
    class Accumulator
    {
    public:
        void Begin() { start_ = std::chrono::steady_clock::now(); }
        void End() { total_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(); }
        void Commit(Metric& metric) { metric.Add(total_); total_ = 0; }

    private:
        std::chrono::steady_clock::time_point start_{};
        double total_ = 0;
    };
}
