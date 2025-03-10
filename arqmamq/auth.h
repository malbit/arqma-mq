#pragma once
#include <iosfwd>
#include <string>
#include <cstring>
#include <unordered_set>

namespace arqmamq {

/// Authentication levels for command categories and connections
enum class AuthLevel {
    denied, ///< Not actually an auth level, but can be returned by the AllowFunc to deny an incoming connection.
    none, ///< No authentication at all; any random incoming ZMQ connection can invoke this command.
    basic, ///< Basic authentication commands require a login, or a node that is specifically configured to be a public node (e.g. for public RPC).
    admin, ///< Advanced authentication commands require an admin user, either via explicit login or by implicit login from localhost.  This typically protects administrative commands like shutting down, starting mining, or access sensitive data.
};

std::ostream& operator<<(std::ostream& os, AuthLevel a);

/// The access level for a command category
struct Access {
    /// Minimum access level required
    AuthLevel auth;
    /// If true only remote SNs may call the category commands
    bool remote_sn;
    /// If true the category requires that the local node is a SN
    bool local_sn;

    Access(AuthLevel auth = AuthLevel::none, bool remote_sn = false, bool local_sn = false)
        : auth{auth}, remote_sn{remote_sn}, local_sn{local_sn} {}
};

struct already_hashed {
  size_t operator()(const std::string& s) const
  {
    if (s.size() < sizeof(size_t))
      return std::hash<std::string>{}(s);
    size_t hash;
    std::memcpy(&hash, &s[0], sizeof(hash));
    return hash;
  }
};

using pubkey_set = std::unordered_set<std::string, already_hashed>;

}
