#pragma once
#include <qt_raster_session.hpp>
#include <functional>
#include <map>
#include <set>

namespace sogen::qt_raster
{
    class web_presenter
    {
      public:
        using output = std::function<void(const Packet&, std::span<const uint8_t>)>;
        using ready = std::function<bool(uint64_t, uint32_t)>;
        enum class input_result
        {
            accepted,
            retry,
            discarded
        };
        web_presenter(output send, ready frame_ready);
        void update(std::span<session* const> sessions);
        static input_result input(std::span<session* const> sessions, std::span<const uint8_t> bytes);
        void clear();

      private:
        using key = std::pair<uint64_t, uint32_t>;

        struct state
        {
            window metadata{};
            std::weak_ptr<const std::vector<uint8_t>> frame{};
        };

        output send_;
        ready ready_;
        uint32_t sequence_{};
        std::map<key, state> windows_{};
        std::set<uint64_t> sessions_{};
        void send(uint64_t identity, uint32_t id, Op op, const std::array<int32_t, 8>& args = {}, std::span<const uint8_t> payload = {});
        size_t depth(key value) const;
    };
}
