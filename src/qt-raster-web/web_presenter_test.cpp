#include "web_presenter.hpp"
#include <gtest/gtest.h>
#include <cstring>

namespace sogen::qt_raster::test
{
    namespace
    {
        struct client
        {
            session value{};
            uint32_t seq{};
            Packet reply{};

            void send(const Op op, const uint32_t id = 0, const std::array<int32_t, 8>& args = {}, std::vector<uint8_t> payload = {})
            {
                Packet packet{};
                packet.op = op;
                packet.id = id;
                packet.seq = ++this->seq;
                packet.session = op == Hello ? 0 : this->value.identity();
                packet.a = args;
                packet.size = static_cast<uint32_t>(payload.size());
                ASSERT_EQ(result::success, this->value.exchange(packet, std::move(payload), 4096, [this](auto bytes) {
                    std::memcpy(&this->reply, bytes.data(), sizeof(this->reply));
                    return true;
                }));
            }

            void create(const uint32_t id, const uint32_t parent = 0)
            {
                this->send(Create, id, {0, 0, 2, 2, 1, 0, static_cast<int32_t>(parent), 1000});
            }

            void paint(const uint32_t id, const uint8_t pixel, const int32_t scale = 1000)
            {
                this->send(Frame, id, {2, 2, 8, scale, 0, 0, 2, 2}, std::vector<uint8_t>(16, pixel));
            }

            Packet poll()
            {
                Packet request{};
                request.op = Poll;
                request.seq = ++this->seq;
                request.session = this->value.identity();
                Packet response{};
                EXPECT_EQ(result::success, this->value.exchange(request, {}, 4096, [&](auto bytes) {
                    std::memcpy(&response, bytes.data(), sizeof(response));
                    return true;
                }));
                return response;
            }
        };

        std::vector<uint8_t> encode(Packet packet, const std::vector<uint8_t>& payload = {})
        {
            packet.size = static_cast<uint32_t>(payload.size());
            std::vector<uint8_t> bytes(sizeof(packet) + payload.size());
            std::memcpy(bytes.data(), &packet, sizeof(packet));
            std::copy(payload.begin(), payload.end(), bytes.begin() + sizeof(packet));
            return bytes;
        }

        struct observer
        {
            std::vector<Packet> packets;
            std::vector<std::vector<uint8_t>> pixels;
            bool ready{true};
            web_presenter presenter{[this](const Packet& packet, auto payload) {
                                        this->packets.push_back(packet);
                                        if (packet.op == Frame)
                                        {
                                            this->pixels.emplace_back(payload.begin(), payload.end());
                                        }
                                    },
                                    [this](auto, auto) { return this->ready; }};

            void update(client& source)
            {
                session* pointer = &source.value;
                this->presenter.update({&pointer, 1});
            }
        };
    }

    TEST(WebRasterPresentation, RetainsFrameScaleAndSessionIdentityOnTheWire)
    {
        client source;
        source.send(Hello);
        source.create(1);
        source.paint(1, 0x7f, 2000);
        observer output;
        output.update(source);
        ASSERT_EQ(1u, output.pixels.size());
        EXPECT_EQ(std::vector<uint8_t>(16, 0x7f), output.pixels.front());
        const auto frame = std::ranges::find_if(output.packets, [](const auto& value) { return value.op == Frame; });
        ASSERT_NE(output.packets.end(), frame);
        EXPECT_EQ(2000, frame->a[3]);
        EXPECT_EQ(source.value.identity(), frame->session);
        EXPECT_TRUE(valid_frame(*frame));
    }

    TEST(WebRasterPresentation, HoldsFramesUntilAcknowledgedAndThenSendsOnlyTheLatest)
    {
        client source;
        source.send(Hello);
        source.create(1);
        source.paint(1, 1);
        observer output;
        output.update(source);
        output.ready = false;
        source.paint(1, 2);
        output.update(source);
        source.paint(1, 3);
        output.update(source);
        ASSERT_EQ(1u, output.pixels.size());
        output.ready = true;
        output.update(source);
        output.update(source);
        ASSERT_EQ(2u, output.pixels.size());
        EXPECT_EQ(std::vector<uint8_t>(16, 3), output.pixels.back());
    }

    TEST(WebRasterPresentation, CreatesParentsFirstAndDestroysChildrenFirst)
    {
        client source;
        source.send(Hello);
        source.create(5);
        source.create(1, 5);
        observer output;
        output.update(source);
        std::vector<uint32_t> creates;
        for (const auto& packet : output.packets)
        {
            if (packet.op == Create)
            {
                creates.push_back(packet.id);
            }
        }
        EXPECT_EQ((std::vector<uint32_t>{5, 1}), creates);
        output.packets.clear();
        source.value.reset();
        output.update(source);
        ASSERT_EQ(3u, output.packets.size());
        EXPECT_EQ(Destroy, output.packets[0].op);
        EXPECT_EQ(1u, output.packets[0].id);
        EXPECT_EQ(Destroy, output.packets[1].op);
        EXPECT_EQ(5u, output.packets[1].id);
        EXPECT_EQ(End, output.packets[2].op);
    }

    TEST(WebRasterPresentation, AFullGuestQueueRetainsTheKeyReleaseForRetry)
    {
        client source;
        source.send(Hello);
        source.create(1);
        observer output;
        Packet movement{};
        movement.op = Mouse;
        movement.id = 1;
        movement.session = source.value.identity();
        movement.a[4] = 5;
        for (size_t i = 0; i < max_events; ++i)
        {
            ASSERT_TRUE(source.value.enqueue_input(movement));
        }
        Packet release{};
        release.op = Key;
        release.id = 1;
        release.session = source.value.identity();
        release.a = {7, 'A'};
        session* pointer = &source.value;
        const auto bytes = encode(release);
        EXPECT_EQ(web_presenter::input_result::retry, web_presenter::input({&pointer, 1}, bytes));
        EXPECT_EQ(Mouse, source.poll().op);
        EXPECT_EQ(web_presenter::input_result::accepted, web_presenter::input({&pointer, 1}, bytes));
        for (size_t i = 1; i < max_events; ++i)
        {
            EXPECT_EQ(Mouse, source.poll().op);
        }
        const auto received = source.poll();
        EXPECT_EQ(Key, received.op);
        EXPECT_EQ(7, received.a[0]);
        EXPECT_EQ('A', received.a[1]);
    }

    TEST(WebRasterPresentation, InvalidAndStaleInputsAreDiscardedInsteadOfBlockingTheQueue)
    {
        client source;
        source.send(Hello);
        source.create(1);
        observer output;
        session* pointer = &source.value;
        Packet packet{};
        packet.op = Key;
        packet.id = 1;
        packet.a = {6, 'A'};
        packet.session = source.value.identity() + 1;
        EXPECT_EQ(web_presenter::input_result::discarded, web_presenter::input({&pointer, 1}, encode(packet)));
        packet.session = source.value.identity();
        EXPECT_EQ(web_presenter::input_result::discarded, web_presenter::input({&pointer, 1}, encode(packet, {0xff})));
        packet.op = Screen;
        EXPECT_EQ(web_presenter::input_result::discarded, web_presenter::input({&pointer, 1}, encode(packet)));
        EXPECT_EQ(Empty, source.poll().op);
    }

    TEST(WebRasterPresentation, AReopenedSessionCannotReceiveThePreviousSessionsInput)
    {
        client source;
        source.send(Hello);
        source.create(1);
        observer output;
        output.update(source);
        Packet stale{};
        stale.op = Close;
        stale.id = 1;
        stale.session = source.value.identity();
        source.value.reset();
        source.seq = 0;
        source.send(Hello);
        source.create(1);
        output.update(source);
        session* pointer = &source.value;
        ASSERT_NE(stale.session, source.value.identity());
        EXPECT_EQ(web_presenter::input_result::discarded, web_presenter::input({&pointer, 1}, encode(stale)));
        EXPECT_EQ(Empty, source.poll().op);
    }

    TEST(WebRasterPresentation, ANewObserverReceivesExistingFocusAndStackingRequests)
    {
        client source;
        source.send(Hello);
        source.create(1);
        source.send(Visible, 1, {1, 0});
        source.send(Raise, 1, {0});
        source.send(Focus, 1, {1});
        observer output;
        output.update(source);
        const auto lowered = std::ranges::find_if(output.packets, [](const auto& value) { return value.op == Raise; });
        ASSERT_NE(output.packets.end(), lowered);
        EXPECT_EQ(0, lowered->a[0]);
        const auto focused = std::ranges::find_if(output.packets, [](const auto& value) { return value.op == Focus; });
        ASSERT_NE(output.packets.end(), focused);
        EXPECT_EQ(1, focused->a[0]);
    }

    TEST(WebRasterPresentation, FocusChangesFollowRequestOrderInsteadOfWindowIdOrder)
    {
        client source;
        source.send(Hello);
        source.create(9);
        source.create(1);
        observer output;
        output.update(source);
        output.packets.clear();
        source.send(Focus, 9, {1});
        source.send(Focus, 1, {1});
        output.update(source);
        std::vector<uint32_t> focus;
        for (const auto& packet : output.packets)
        {
            if (packet.op == Focus)
            {
                focus.push_back(packet.id);
            }
        }
        EXPECT_EQ((std::vector<uint32_t>{9, 1}), focus);
    }

    TEST(WebRasterPresentation, ScreenMetricsAreAvailableInTheInitialHandshake)
    {
        client source;
        ASSERT_TRUE(source.value.configure_screen({2560, 1792, 2000}));
        source.send(Hello);
        EXPECT_EQ(2560, source.reply.a[0]);
        EXPECT_EQ(1792, source.reply.a[1]);
        EXPECT_EQ(2000, source.reply.a[2]);
        EXPECT_FALSE(source.value.configure_screen({1280, 900, 1000}));
        source.send(Create, 1, {0, 0, 760, 1100, 1, 0, 0, 2000});
        observer output;
        output.update(source);
        const auto created = std::ranges::find_if(output.packets, [](const auto& value) { return value.op == Create; });
        ASSERT_NE(output.packets.end(), created);
        EXPECT_EQ(2000, created->a[7]);
        EXPECT_EQ(760, created->a[2]);
    }

}
