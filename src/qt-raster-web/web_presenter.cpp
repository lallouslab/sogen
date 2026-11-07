#include "web_presenter.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sogen::qt_raster
{
    web_presenter::web_presenter(output send, ready frame_ready)
        : send_(std::move(send)),
          ready_(std::move(frame_ready))
    {
    }

    void web_presenter::send(const uint64_t identity, const uint32_t id, const Op op, const std::array<int32_t, 8>& args,
                             const std::span<const uint8_t> payload)
    {
        if (this->sequence_ == UINT32_MAX)
        {
            throw std::runtime_error("Raster presentation sequence exhausted");
        }
        Packet packet{};
        packet.op = op;
        packet.session = identity;
        packet.id = id;
        packet.seq = ++this->sequence_;
        packet.a = args;
        packet.size = static_cast<uint32_t>(payload.size());
        this->send_(packet, payload);
    }

    size_t web_presenter::depth(key value) const
    {
        size_t result{};
        for (; value.second && result <= this->windows_.size(); ++result)
        {
            const auto it = this->windows_.find(value);
            if (it == this->windows_.end())
            {
                break;
            }
            value.second = it->second.metadata.parent;
        }
        return result;
    }

    void web_presenter::update(const std::span<session* const> sessions)
    {
        std::map<key, const window*> live;
        std::set<uint64_t> identities;
        size_t bytes{};
        for (const auto* source : sessions)
        {
            if (!source->active())
            {
                continue;
            }
            identities.insert(source->identity());
            for (const auto& [id, value] : source->windows())
            {
                live.emplace(key{source->identity(), id}, &value);
                if (value.surface)
                {
                    bytes += value.surface->pixels->size();
                }
            }
        }
        if (identities.size() > 16 || live.size() > max_windows || bytes > max_payload)
        {
            throw std::runtime_error("Browser raster presentation budget exceeded");
        }
        std::vector<key> removed;
        for (const auto& [id, state] : this->windows_)
        {
            (void)state;
            if (!live.contains(id))
            {
                removed.push_back(id);
            }
        }
        std::ranges::sort(removed, [&](const auto& a, const auto& b) { return this->depth(a) > this->depth(b); });
        for (const auto& id : removed)
        {
            this->send(id.first, id.second, Destroy);
            this->windows_.erase(id);
        }
        for (const auto identity : this->sessions_)
        {
            if (!identities.contains(identity))
            {
                this->send(identity, 0, End);
            }
        }
        for (const auto identity : identities)
        {
            if (!this->sessions_.contains(identity))
            {
                this->send(identity, 0, Hello);
            }
        }
        this->sessions_ = std::move(identities);
        std::vector<key> ordered;
        std::set<key> created;
        for (const auto& [id, value] : live)
        {
            if (!this->windows_.contains(id))
            {
                created.insert(id);
                this->windows_[id].metadata = *value;
                this->windows_[id].metadata.surface.reset();
            }
            ordered.push_back(id);
        }
        std::ranges::sort(ordered, [&](const auto& a, const auto& b) { return this->depth(a) < this->depth(b); });

        struct request
        {
            key target;
            uint32_t order;
            Op operation;
            int32_t value;
        };

        std::vector<request> requests;
        for (const auto& id : ordered)
        {
            const auto& value = *live.at(id);
            auto& old = this->windows_.at(id);
            const bool added = created.contains(id);
            const auto text = [](const std::string& s) { return std::span{reinterpret_cast<const uint8_t*>(s.data()), s.size()}; };
            if (added)
            {
                this->send(id.first, id.second, Create,
                           {value.geometry[0], value.geometry[1], value.geometry[2], value.geometry[3], value.type, value.modality,
                            static_cast<int32_t>(value.parent), value.scale},
                           text(value.title));
            }
            if (!added && old.metadata.geometry != value.geometry)
            {
                this->send(id.first, id.second, Geometry, {value.geometry[0], value.geometry[1], value.geometry[2], value.geometry[3]});
            }
            if (!added && old.metadata.title != value.title)
            {
                this->send(id.first, id.second, Title, {}, text(value.title));
            }
            if (added || old.metadata.visibility_order != value.visibility_order)
            {
                this->send(id.first, id.second, Visible, {value.visible ? 1 : 0, value.modality});
            }
            if ((added && value.stacking_order != 0) || old.metadata.stacking_order != value.stacking_order)
            {
                requests.push_back({.target = id, .order = value.stacking_order, .operation = Raise, .value = value.raised ? 1 : 0});
            }
            if ((added && value.focus_order != 0) || old.metadata.focus_order != value.focus_order)
            {
                requests.push_back({.target = id, .order = value.focus_order, .operation = Focus, .value = value.focused ? 1 : 0});
            }
            if (added || old.metadata.cursor != value.cursor)
            {
                this->send(id.first, id.second, Cursor, {value.cursor});
            }
            if (value.surface && old.frame.lock() != value.surface->pixels && this->ready_(id.first, id.second))
            {
                const auto& frame = *value.surface;
                this->send(id.first, id.second, Frame,
                           {frame.width, frame.height, frame.stride, frame.scale, 0, 0, frame.width, frame.height}, *frame.pixels);
                old.frame = frame.pixels;
            }
            old.metadata = value;
            old.metadata.surface.reset();
        }
        std::ranges::sort(
            requests, [](const auto& a, const auto& b) { return std::pair{a.target.first, a.order} < std::pair{b.target.first, b.order}; });
        for (const auto& value : requests)
        {
            this->send(value.target.first, value.target.second, value.operation, {value.value});
        }
    }

    web_presenter::input_result web_presenter::input(const std::span<session* const> sessions, const std::span<const uint8_t> bytes)
    {
        if (bytes.size() < sizeof(Packet) || bytes.size() > sizeof(Packet) + max_input_payload)
        {
            return input_result::discarded;
        }
        Packet packet{};
        std::memcpy(&packet, bytes.data(), sizeof(packet));
        if (!valid(packet, bytes.size()))
        {
            return input_result::discarded;
        }
        for (auto* source : sessions)
        {
            if (!source->active() || source->identity() != packet.session)
            {
                continue;
            }
            if (packet.op != Screen && !source->windows().contains(packet.id))
            {
                return input_result::discarded;
            }
            if (packet.op < Mouse || packet.op > Screen)
            {
                return input_result::discarded;
            }
            if (source->input_queue_full())
            {
                return input_result::retry;
            }
            return source->enqueue_input(packet, bytes.subspan(sizeof(Packet))) ? input_result::accepted : input_result::discarded;
        }
        return input_result::discarded;
    }

    void web_presenter::clear()
    {
        this->update({});
    }
}
