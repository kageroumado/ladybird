/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibCore/System.h>
#include <UI/Refrax/Wire.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace Refrax {

ErrorOr<NonnullOwnPtr<Wire>> Wire::adopt(int fd)
{
    auto flags = TRY(Core::System::fcntl(fd, F_GETFL));
    TRY(Core::System::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
    TRY(Core::System::fcntl(fd, F_SETFD, FD_CLOEXEC));

    auto wire = adopt_own(*new Wire(fd));
    wire->m_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    wire->m_notifier->on_activation = [wire = wire.ptr()] {
        wire->read_available();
    };
    return wire;
}

Wire::Wire(int fd)
    : m_fd(fd)
{
}

Wire::~Wire()
{
    if (m_notifier)
        m_notifier->close();
    if (m_fd >= 0)
        ::close(m_fd);
}

void Wire::close()
{
    if (m_fd < 0)
        return;
    m_notifier->close();
    ::close(m_fd);
    m_fd = -1;
    if (on_close)
        on_close();
}

void Wire::send(StringView name, JsonObject fields)
{
    if (m_fd < 0)
        return;

    JsonObject message;
    message.set(name, move(fields));
    auto text = message.serialized();
    auto bytes = text.bytes();

    u8 length[4] = {
        static_cast<u8>(bytes.size() >> 24),
        static_cast<u8>(bytes.size() >> 16),
        static_cast<u8>(bytes.size() >> 8),
        static_cast<u8>(bytes.size()),
    };

    auto write_all = [&](ReadonlyBytes remaining) {
        while (!remaining.is_empty()) {
            auto written = ::write(m_fd, remaining.data(), remaining.size());
            if (written > 0) {
                remaining = remaining.slice(written);
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0 && errno == EAGAIN) {
                // The client reads on its main thread, so a full socket empties as soon as that thread comes around.
                pollfd poll_fd { .fd = m_fd, .events = POLLOUT, .revents = 0 };
                (void)::poll(&poll_fd, 1, -1);
                continue;
            }
            return false;
        }
        return true;
    };

    if (!write_all({ length, sizeof(length) }) || !write_all(bytes))
        close();
}

void Wire::read_available()
{
    for (;;) {
        u8 chunk[64 * KiB];
        auto count = ::read(m_fd, chunk, sizeof(chunk));
        if (count > 0) {
            m_received.append(chunk, count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN)
            break;
        // The client is gone, or the socket is broken.
        close();
        return;
    }

    size_t offset = 0;
    while (m_received.size() - offset >= 4) {
        auto const* header = m_received.data() + offset;
        size_t length = (static_cast<size_t>(header[0]) << 24) | (static_cast<size_t>(header[1]) << 16) | (static_cast<size_t>(header[2]) << 8) | header[3];
        if (length > MAXIMUM_MESSAGE_SIZE) {
            close();
            return;
        }
        if (m_received.size() - offset - 4 < length)
            break;

        auto text = StringView { m_received.data() + offset + 4, length };
        offset += 4 + length;

        auto value = JsonValue::from_string(text);
        if (value.is_error() || !value.value().is_object()) {
            warnln("Refrax host: dropping a message that is not a JSON object");
            continue;
        }
        value.value().as_object().for_each_member([&](String const& name, JsonValue const& fields) {
            if (fields.is_object() && on_message)
                on_message(name, fields.as_object());
        });
        // NB: Handling a message can close the connection.
        if (m_fd < 0)
            return;
    }

    if (offset > 0) {
        auto remaining = m_received.size() - offset;
        if (remaining > 0)
            __builtin_memmove(m_received.data(), m_received.data() + offset, remaining);
        m_received.resize(remaining);
    }
}

}
