/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/Notifier.h>

namespace Refrax {

// The host's connection to the engine client in Refrax's process: a socket carrying JSON objects, each after its length
// as four bytes, most significant first. An object has one key, the name of the message, whose value holds its fields.
class Wire {
    AK_MAKE_NONCOPYABLE(Wire);
    AK_MAKE_NONMOVABLE(Wire);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<Wire>> adopt(int fd);
    ~Wire();

    void send(StringView name, JsonObject fields);

    Function<void(String const& name, JsonObject const& fields)> on_message;
    Function<void()> on_close;

private:
    explicit Wire(int fd);

    void read_available();
    void close();

    // The largest message either side sends. A frame claiming more is a broken stream.
    static constexpr size_t MAXIMUM_MESSAGE_SIZE = 64 * MiB;

    int m_fd { -1 };
    RefPtr<Core::Notifier> m_notifier;
    ByteBuffer m_received;
};

}
