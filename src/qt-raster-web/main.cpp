#include "web_presenter.hpp"
#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <devices/qt_raster.hpp>
#include <emscripten.h>

// clang-format off
EM_JS(void, send_packet, (const void* header, const void* pixels, int size), {
    const bytes = new Uint8Array(64 + size);
    bytes.set(HEAPU8.subarray(header, header + 64));
    if (size) bytes.set(HEAPU8.subarray(pixels, pixels + size), 64);
    globalThis.sogenRasterHeapBytes = HEAPU8.byteLength;
    globalThis.sogenRasterSend(bytes);
});
EM_JS(int, frame_ready, (uint32_t low, uint32_t high, uint32_t id), {
    return globalThis.sogenRasterReady(low, high, id);
});
EM_JS(int, poll_input, (void* target), {
    const bytes = globalThis.sogenRasterInput[0];
    if (!bytes) return 0;
    HEAPU8.set(bytes, target); return bytes.length;
});
EM_JS(void, consume_input, (), { globalThis.sogenRasterInput.shift(); });
EM_JS(int, screen_metric, (int index), { return globalThis.sogenRasterScreen[index]; });
EM_JS(void, guest_output, (const char* data, int size), { globalThis.sogenRasterStdout(UTF8ToString(data, size)); });
EM_JS(void, finished, (uint32_t status, const char* error), { globalThis.sogenRasterDone(status, error ? UTF8ToString(error) : null); });
// clang-format on

namespace
{
    class pumping_backend final : public sogen::ui_backend
    {
      public:
        std::function<void()> refresh{};

        void set_event_sink(event_sink) override
        {
        }

        void pump_events() override
        {
            emscripten_sleep(0);
            if (this->refresh)
            {
                this->refresh();
            }
        }
    };
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
        {
            throw std::invalid_argument("Expected an emulation root and guest application");
        }
        auto backend = std::make_unique<pumping_backend>();
        auto* pumping = backend.get();
        sogen::emulator_interfaces interfaces{};
        interfaces.ui = std::move(backend);
        interfaces.audio = std::make_unique<sogen::null_audio_backend>();
        interfaces.console = std::make_unique<sogen::null_console_backend>();
        sogen::application_settings application{.application = argv[2]};
        application.arguments = {u"-platform", u"bridge"};
        sogen::emulator_callbacks callbacks{};
        callbacks.on_stdout = [](const std::string_view text) { guest_output(text.data(), static_cast<int>(text.size())); };
        callbacks.on_ioctrl = [](sogen::io_device& device, std::u16string_view, uint32_t) {
            auto* raster = dynamic_cast<sogen::qt_raster_device*>(&device);
            if (raster && !raster->session().active() &&
                !raster->session().configure_screen({screen_metric(0), screen_metric(1), screen_metric(2)}))
            {
                throw std::runtime_error("Invalid browser screen metrics");
            }
        };
        sogen::windows_emulator emulator{sogen::create_x86_64_emulator(sogen::backend_type::unicorn), std::move(application),
                                         sogen::emulator_settings{.disable_logging = true,
                                                                  .use_instruction_precision = false,
                                                                  .emulation_root = argv[1],
                                                                  .root_mode = sogen::filesystem_mode::read_write},
                                         std::move(callbacks), std::move(interfaces)};
        sogen::qt_raster::web_presenter presenter{
            [](const auto& packet, const auto payload) { send_packet(&packet, payload.data(), static_cast<int>(payload.size())); },
            [](const uint64_t session, const uint32_t id) {
                return frame_ready(static_cast<uint32_t>(session), static_cast<uint32_t>(session >> 32), id) != 0;
            }};
        pumping->refresh = [&] {
            emulator.synchronize([&] {
                std::vector<sogen::qt_raster::session*> sessions;
                for (auto& [id, device] : emulator.process.devices)
                {
                    (void)id;
                    if (auto* raster = device.get_internal_device<sogen::qt_raster_device>(); raster && raster->session().active())
                    {
                        sessions.push_back(&raster->session());
                    }
                }
                presenter.update(sessions);
                std::array<uint8_t, 4096> bytes{};
                for (size_t count = 0; count < 256; ++count)
                {
                    const auto size = poll_input(bytes.data());
                    if (size == 0)
                    {
                        break;
                    }
                    if (size < 0 || size > static_cast<int>(bytes.size()))
                    {
                        throw std::runtime_error("Invalid browser input packet");
                    }
                    if (sogen::qt_raster::web_presenter::input(sessions, {bytes.data(), static_cast<size_t>(size)}) ==
                        sogen::qt_raster::web_presenter::input_result::retry)
                    {
                        break;
                    }
                    consume_input();
                }
            });
        };
        emulator.start();
        pumping->refresh = {};
        presenter.clear();
        const auto status = emulator.process.exit_status.value_or(uint32_t{1});
        finished(status, nullptr);
        return status == 0 ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        finished(1, error.what());
        return 1;
    }
}
