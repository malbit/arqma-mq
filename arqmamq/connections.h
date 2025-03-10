#pragma once
#include "auth.h"
#include "bt_value.h"
#include <string_view>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace arqmamq {

struct ConnectionID;

namespace detail {
template <typename... T>
bt_dict build_send(ConnectionID to, std::string_view cmd, T&&... opts);
}

/// Opaque data structure representing a connection which supports ==, !=, < and std::hash.  For
/// connections to service node this is the service node pubkey (and you can pass a 32-byte string
/// anywhere a ConnectionID is called for).  For non-SN remote connections you need to keep a copy
/// of the ConnectionID returned by connect_remote().
struct ConnectionID {
    ConnectionID() : ConnectionID(0) {}
    ConnectionID(std::string pubkey_) : id{SN_ID}, pk{std::move(pubkey_)} {
        if (pk.size() != 32)
            throw std::runtime_error{"Invalid pubkey: expected 32 bytes"};
    }
    ConnectionID(std::string_view pubkey_) : ConnectionID(std::string{pubkey_}) {}
    ConnectionID(const ConnectionID&) = default;
    ConnectionID(ConnectionID&&) = default;
    ConnectionID& operator=(const ConnectionID&) = default;
    ConnectionID& operator=(ConnectionID&&) = default;

    // Returns true if this is a ConnectionID (false for a default-constructed, invalid id)
    explicit operator bool() const {
        return id != 0;
    }

    bool operator==(const ConnectionID &o) const {
        if (sn() && o.sn())
            return pk == o.pk;
        return id == o.id && route == o.route;
    }
    bool operator!=(const ConnectionID &o) const { return !(*this == o); }
    bool operator<(const ConnectionID &o) const {
        if (sn() && o.sn())
            return pk < o.pk;
        return id < o.id || (id == o.id && route < o.route);
    }
    // Returns true if this ConnectionID represents a SN connection
    bool sn() const { return id == SN_ID; }

    const std::string& pubkey() const { return pk; }

    ConnectionID unrouted() { return ConnectionID{id, pk, ""}; }

private:
    ConnectionID(long long id) : id{id} {}
    ConnectionID(long long id, std::string pubkey, std::string route = "")
        : id{id}, pk{std::move(pubkey)}, route{std::move(route)} {}

    constexpr static long long SN_ID = -1;
    long long id = 0;
    std::string pk;
    std::string route;
    friend class ArqmaMQ;
    friend struct std::hash<ConnectionID>;
    template <typename... T>
    friend bt_dict detail::build_send(ConnectionID to, std::string_view cmd, T&&... opts);
    friend std::ostream& operator<<(std::ostream& o, const ConnectionID& conn);
};

} // namespace arqmamq
namespace std {
    template <> struct hash<arqmamq::ConnectionID> {
        size_t operator()(const arqmamq::ConnectionID &c) const {
            return c.sn() ? arqmamq::already_hashed{}(c.pk) :
                std::hash<long long>{}(c.id) + std::hash<std::string>{}(c.route);
        }
    };
} // namespace std

