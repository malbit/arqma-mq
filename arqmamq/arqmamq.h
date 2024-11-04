// Copyright (c) 2019-2020, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <string>
#include <list>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>
#include <chrono>
#include <atomic>
#include <cassert>
#include <zmq.hpp>
#include "bt_serialize.h"
#include "string_view.h"

#if ZMQ_VERSION < ZMQ_MAKE_VERSION (4, 3, 0)
// Timers were not added until 4.3.0
#error "ZMQ >= 4.3.0 required"
#endif

namespace arqmamq {

using namespace std::literals;

/// Logging levels passed into LogFunc.  (Note that trace does nothing more than debug in a release
/// build).
enum class LogLevel { fatal, error, warn, info, debug, trace };

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
    AuthLevel auth = AuthLevel::none;
    /// If true only remote SNs may call the category commands
    bool remote_sn = false;
    /// If true the category requires that the local node is a SN
    bool local_sn = false;
};

/// Return type of the AllowFunc: this determines whether we allow the connection at all, and if so,
/// sets the initial authentication level and tells ArqmaMQ whether the other end is an active SN.
struct Allow {
    AuthLevel auth = AuthLevel::none;
    bool remote_sn = false;
};

class ArqmaMQ;

/// Opaque data structure representing a connection which supports ==, !=, < and std::hash.  For
/// connections to service node this is the service node pubkey (and you can pass a 32-byte string
/// anywhere a ConnectionID is called for).  For non-SN remote connections you need to keep a copy
/// of the ConnectionID returned by connect_remote().
struct ConnectionID {
    ConnectionID(std::string pubkey_) : id{SN_ID}, pk{std::move(pubkey_)} {
        if (pk.size() != 32)
            throw std::runtime_error{"Invalid pubkey: expected 32 bytes"};
    }
    ConnectionID(string_view pubkey_) : ConnectionID(std::string{pubkey_}) {}
    ConnectionID(const ConnectionID&) = default;
    ConnectionID(ConnectionID&&) = default;
    ConnectionID& operator=(const ConnectionID&) = default;
    ConnectionID& operator=(ConnectionID&&) = default;

    // Returns true if this is a ConnectionID (false for a default-constructed, invalid id)
    explicit operator bool() const {
        return id != 0;
    }

    // Two ConnectionIDs are equal if they are both SNs and have matching pubkeys, or they are both
    // not SNs and have matching internal IDs.  (Pubkeys do not have to match for non-SNs, and
    // routes are not considered for equality at all).
    bool operator==(const ConnectionID &o) const {
        if (id == SN_ID && o.id == SN_ID)
            return pk == o.pk;
        return id == o.id;
    }
    bool operator!=(const ConnectionID &o) const { return !(*this == o); }
    bool operator<(const ConnectionID &o) const {
        if (id == SN_ID && o.id == SN_ID)
            return pk < o.pk;
        return id < o.id;
    }
    // Returns true if this ConnectionID represents a SN connection
    bool sn() const { return id == SN_ID; }

    // Returns this connection's pubkey, if any.  (Note that it is possible to have a pubkey and not
    // be a SN when connecting to secure remotes: having a non-empty pubkey does not imply that
    // `sn()` is true).
    const std::string& pubkey() const { return pk; }
    // Default construction; creates a ConnectionID with an invalid internal ID that will not match
    // an actual connection.
    ConnectionID() : ConnectionID(0) {}
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
    friend bt_dict build_send(ConnectionID to, string_view cmd, const T&... opts);
    friend std::ostream& operator<<(std::ostream& o, const ConnectionID& conn);
};

} // namespace arqmamq
namespace std {
    // Need this here because we stick it in an unordered_map below.
    template <> struct hash<arqmamq::ConnectionID> {
        size_t operator()(const arqmamq::ConnectionID &c) const {
            return c.sn() ? std::hash<std::string>{}(c.pk) :
                std::hash<long long>{}(c.id);
        }
    };
} // namespace std
namespace arqmamq {

/// Encapsulates an incoming message from a remote connection with message details plus extra
/// info need to send a reply back through the proxy thread via the `reply()` method.  Note that
/// this object gets reused: callbacks should use but not store any reference beyond the callback.
class Message {
public:
    ArqmaMQ& arqmamq; ///< The owning ArqmaMQ object
    std::vector<string_view> data; ///< The provided command data parts, if any.
    ConnectionID conn; ///< The connection info for routing a reply; also contains the pubkey/sn status.
    std::string reply_tag; ///< If the invoked command is a request command this is the required reply tag that will be prepended by `send_reply()`.

    /// Constructor
    Message(ArqmaMQ& arqmq, ConnectionID cid) : arqmamq{arqmq}, conn{std::move(cid)} {}

    // Non-copyable
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    /// Sends a command back to whomever sent this message.  Arguments are forwarded to send() but
    /// with send_option::optional{} added if the originator is not a SN.  For SN messages (i.e.
    /// where `sn` is true) this is a "strong" reply by default in that the proxy will attempt to
    /// establish a new connection to the SN if no longer connected.  For non-SN messages the reply
    /// will be attempted using the available routing information, but if the connection has already
    /// been closed the reply will be dropped.
    ///
    /// If you want to send a non-strong reply even when the remote is a service node then add
    /// an explicit `send_option::optional()` argument.
    template <typename... Args>
    void send_back(string_view, Args&&... args);

    /// Sends a reply to a request.  This takes no command: the command is always the built-in
    /// "REPLY" command, followed by the unique reply tag, then any reply data parts.  All other
    /// arguments are as in `send_back()`.  You should only send one reply for a command expecting
    /// replies, though this is not enforced: attempting to send multiple replies will simply be
    /// dropped when received by the remote.  (Note, however, that it is possible to send multiple
    /// messages -- e.g. you could send a reply and then also call send_back() and/or send_request()
    /// to send more requests back to the sender).
    template <typename... Args>
    void send_reply(Args&&... args);

    /// Sends a request back to whomever sent this message.  This is effectively a wrapper around
    /// arqmq.request() that takes care of setting up the recipient arguments.
    template <typename ReplyCallback, typename... Args>
    void send_request(string_view cmd, ReplyCallback&& callback, Args&&... args);
};

// Forward declarations; see batch.h
namespace detail { class Batch; }
template <typename R> class Batch;

/** The keep-alive time for a send() that results in a establishing a new outbound connection.  To
 * use a longer keep-alive to a host call `connect()` first with the desired keep-alive time or pass
 * the send_option::keep_alive.
 */
static constexpr auto DEFAULT_SEND_KEEP_ALIVE = 30s;

// How frequently we cleanup connections (closing idle connections, calling connect or request failure callbacks)
static constexpr auto CONN_CHECK_INTERVAL = 1s;

// The default timeout for connect_remote()
static constexpr auto REMOTE_CONNECT_TIMEOUT = 10s;

// The minimum amount of time we wait for a reply to a REQUEST before calling the callback with
// `false` to signal a timeout.
static constexpr auto REQUEST_TIMEOUT = 15s;

/// Maximum length of a category
static constexpr size_t MAX_CATEGORY_LENGTH = 50;

/// Maximum length of a command
static constexpr size_t MAX_COMMAND_LENGTH = 200;

class CatHelper;

/**
 * Class that handles ArqmaMQ listeners, connections, proxying, and workers.  An application
 * typically has just one instance of this class.
 */
class ArqmaMQ {

private:

    /// The global context
    zmq::context_t context;

    /// A unique id for this ArqmaMQ instance, assigned in a thread-safe manner during construction.
    const int object_id;

    /// The x25519 keypair of this connection.  For service nodes these are the long-run x25519 keys
    /// provided at construction, for non-service-node connections these are generated during
    /// construction.
    std::string pubkey, privkey;

    /// True if *this* node is running in service node mode (whether or not actually active)
    bool local_service_node = false;

    /// The thread in which most of the intermediate work happens (handling external connections
    /// and proxying requests between them to worker threads)
    std::thread proxy_thread;

    /// Will be true (and is guarded by a mutex) if the proxy thread is quitting; guards against new
    /// control sockets from threads trying to talk to the proxy thread.
    bool proxy_shutting_down = false;

    /// Called to obtain a "command" socket that attaches to `control` to send commands to the
    /// proxy thread from other threads.  This socket is unique per thread and ArqmaMQ instance.
    zmq::socket_t& get_control_socket();

    /// Stores all of the sockets created in different threads via `get_control_socket`.  This is
    /// only used during destruction to close all of those open sockets, and is protected by an
    /// internal mutex which is only locked by new threads getting a control socket and the
    /// destructor.
    std::vector<std::shared_ptr<zmq::socket_t>> thread_control_sockets;

public:

    /// Callback type invoked to determine whether the given new incoming connection is allowed to
    /// connect to us and to set its initial authentication level.
    ///
    /// @param ip - the ip address of the incoming connection
    /// @param pubkey - the x25519 pubkey of the connecting client (32 byte string).  Note that this
    /// will only be non-empty for incoming connections on `listen_curve` sockets; `listen_plain`
    /// sockets do not have a pubkey.
    ///
    /// @returns an `AuthLevel` enum value indicating the default auth level for the incoming
    /// connection, or AuthLevel::denied if the connection should be refused.
    using AllowFunc = std::function<Allow(string_view ip, string_view pubkey)>;

    /// Callback that is invoked when we need to send a "strong" message to a SN that we aren't
    /// already connected to and need to establish a connection.  This callback returns the ZMQ
    /// connection string we should use which is typically a string such as `tcp://1.2.3.4:5678`.
    using SNRemoteAddress = std::function<std::string(string_view pubkey)>;

    /// The callback type for registered commands.
    using CommandCallback = std::function<void(Message& message)>;

    /// The callback for making requests.  This is called with `true` and a (moved) vector of data
    /// part strings when we get a reply, or `false` and empty vector on timeout.
    using ReplyCallback = std::function<void(bool success, std::vector<std::string> data)>;

    /// Called to write a log message.  This will only be called if the `level` is >= the current
    /// ArqmaMQ object log level.  It must be a raw function pointer (or a capture-less lambda) for
    /// performance reasons.  Takes four arguments: the log level of the message, the filename and
    /// line number where the log message was invoked, and the log message itself.
    using Logger = std::function<void(LogLevel level, const char* file, int line, std::string msg)>;

    /// Callback for the success case of connect_remote()
    using ConnectSuccess = std::function<void(ConnectionID)>;
    /// Callback for the failure case of connect_remote()
    using ConnectFailure = std::function<void(ConnectionID, string_view)>;

    /// Explicitly non-copyable, non-movable because most things here aren't copyable, and a few
    /// things aren't movable, either.  If you need to pass the ArqmaMQ instance around, wrap it
    /// in a unique_ptr or shared_ptr.
    ArqmaMQ(const ArqmaMQ&) = delete;
    ArqmaMQ& operator=(const ArqmaMQ&) = delete;
    ArqmaMQ(ArqmaMQ&&) = delete;
    ArqmaMQ& operator=(ArqmaMQ&&) = delete;

    /** How long to wait for handshaking to complete on external connections before timing out and
     * closing the connection.  Setting this only affects new outgoing connections. */
    std::chrono::milliseconds HANDSHAKE_TIME = 10s;

    /** Whether to use a zmq routing ID based on the pubkey for new outgoing connections.  This is
     * normally desirable as it allows the listener to recognize that the incoming connection is a
     * reconnection from the same remote and handover routing to the new socket while closing off
     * the (likely dead) old socket.  This, however, prevents a single ArqmaMQ instance from
     * establishing multiple connections to the same listening ArqmaMQ, which is sometimes useful
     * (for example when testing), and so this option can be overridden to `false` to use completely
     * random zmq routing ids on outgoing connections (which will thus allow multiple connections).
     */
    bool PUBKEY_BASED_ROUTING_ID = true;

    /** Maximum incoming message size; if a remote tries sending a message larger than this they get
     * disconnected. -1 means no limit. */
    int64_t MAX_MSG_SIZE = 1 * 1024 * 1024;

    /** How long (in ms) to linger sockets when closing them; this is the maximum time zmq spends
     * trying to sending pending messages before dropping them and closing the underlying socket
     * after the high-level zmq socket is closed. */
    std::chrono::milliseconds CLOSE_LINGER = 5s;

private:

    /// The lookup function that tells us where to connect to a peer, or empty if not found.
    SNRemoteAddress sn_lookup;

    /// The log level; this is atomic but we use relaxed order to set and access it (so changing it
    /// might not be instantly visible on all threads, but that's okay).
    std::atomic<LogLevel> log_lvl{LogLevel::warn};

    /// The callback to call with log messages
    Logger logger;

    /// Logging implementation
    template <typename... T>
    void log_(LogLevel lvl, const char* filename, int line, const T&... stuff);

    ///////////////////////////////////////////////////////////////////////////////////
    /// NB: The following are all the domain of the proxy thread (once it is started)!

    /// The socket we listen on for handling ZAP authentication requests (the other end is internal
    /// to zmq which sends requests to us as needed).
    zmq::socket_t zap_auth{context, zmq::socket_type::rep};

    struct bind_data {
        bool curve;
        size_t index;
        AllowFunc allow;
        bind_data(bool curve, AllowFunc allow)
            : curve{curve}, index{0}, allow{std::move(allow)} {}
    };

    /// Addresses on which we are listening (or, before start(), on which we will listen).
    std::vector<std::pair<std::string, bind_data>> bind;

    /// Info about a peer's established connection with us.  Note that "established" means both
    /// connected and authenticated.
    struct peer_info {
        /// Pubkey of the remote, if this connection is a curve25519 connection; empty otherwise.
        std::string pubkey;

        /// True if we've authenticated this peer as a service node.  This gets set on incoming
        /// messages when we check the remote's pubkey, and immediately on outgoing connections to
        /// SNs (since we know their pubkey -- we'll fail to connect if it doesn't match).
        bool service_node = false;

        /// The auth level of this peer, as returned by the AllowFunc for incoming connections or
        /// specified during outgoing connections.
        AuthLevel auth_level = AuthLevel::none;

        /// The actual internal socket index through which this connection is established
        size_t conn_index;

        /// Will be set to a non-empty routing prefix *if* one is necessary on the connection.  This
        /// is used only for SN peers (non-SN incoming connections don't have a peer_info record,
        /// and outgoing connections don't have a route).
        std::string route;

        /// Returns true if this is an outgoing connection.  (This is simply an alias for
        /// route.empty() -- outgoing connections never have a route, incoming connections always
        /// do).
        bool outgoing() const { return route.empty(); }

        /// The last time we sent or received a message (or had some other relevant activity) on
        /// this connection.  Used for closing outgoing connections that have reached an inactivity
        /// expiry time (closing inactive conns for incoming connections is done by the other end).
        std::chrono::steady_clock::time_point last_activity;

        /// Updates last_activity to the current time
        void activity() { last_activity = std::chrono::steady_clock::now(); }

        /// After more than this much inactivity we will close an idle (outgoing) connection
        std::chrono::milliseconds idle_expiry;
    };

    /// Currently peer connections: id -> peer_info.  The ID is as returned by connect_remote or a
    /// SN pubkey string.
    std::unordered_multimap<ConnectionID, peer_info> peers;

    /// Maps connection indices (which can change) to ConnectionIDs (which are permanent).
    std::vector<ConnectionID> conn_index_to_id;

    /// Maps listening socket ConnectionIDs to connection index values (these don't have peers
    /// entries)
    std::unordered_map<ConnectionID, size_t> incoming_conn_index;

    /// The next ConnectionID value we should use (for non-SN connections).
    std::atomic<long long> next_conn_id{1};

    /// Remotes we are still trying to connect to (via connect_remote(), not connect_sn()); when
    /// we pass handshaking we move them out of here and (if set) trigger the on_connect callback.
    /// Unlike regular node-to-node peers, these have an extra "HI"/"HELLO" sequence that we used
    /// before we consider ourselves connected to the remote.
    std::vector<std::tuple<size_t /*conn_index*/, long long /*conn_id*/, std::chrono::steady_clock::time_point, ConnectSuccess, ConnectFailure>> pending_connects;

    /// Pending requests that have been sent out but not yet received a matching "REPLY".  The value
    /// is the timeout timestamp.
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, ReplyCallback>>
        pending_requests;

    /// different polling sockets the proxy handler polls: this always contains some internal
    /// sockets for inter-thread communication followed by a pollitem for every connection (both
    /// incoming and outgoing) in `connections`.  We rebuild this from `connections` whenever
    /// `pollitems_stale` is set to true.
    std::vector<zmq::pollitem_t> pollitems;

    /// If set then rebuild pollitems before the next poll (set when establishing new connections or
    /// closing existing ones).
    bool pollitems_stale = true;

    /// Rebuilds pollitems to include the internal sockets + all incoming/outgoing sockets.
    void rebuild_pollitems();

    /// The connections to/from remotes we currently have open, both listening and outgoing.  Each
    /// element [i] here corresponds to an the pollitem_t at pollitems[i+1+poll_internal_size].
    /// (Ideally we'd use one structure, but zmq requires the pollitems be in contiguous storage).
    std::vector<zmq::socket_t> connections;

    /// Socket we listen on to receive control messages in the proxy thread. Each thread has its own
    /// internal "control" connection (returned by `get_control_socket()`) to this socket used to
    /// give instructions to the proxy such as instructing it to initiate a connection to a remote
    /// or send a message.
    zmq::socket_t command{context, zmq::socket_type::router};

    /// Timers.  TODO: once cppzmq adds an interface around the zmq C timers API then switch to it.
    struct TimersDeleter { void operator()(void* timers); };
    std::unordered_map<int, std::tuple<std::function<void()>, bool, bool>> timer_jobs; // id => {func, squelch, running}
    std::unique_ptr<void, TimersDeleter> timers;
public:
    // This needs to be public because we have to be able to call it from a plain C function.
    // Nothing external may call it!
    void _queue_timer_job(int);
private:

    /// Router socket to reach internal worker threads from proxy
    zmq::socket_t workers_socket{context, zmq::socket_type::router};

    /// indices of idle, active workers
    std::vector<unsigned int> idle_workers;

    /// Maximum number of general task workers, specified by g`/during construction
    int general_workers = std::max<int>(1, std::thread::hardware_concurrency());

    /// Maximum number of possible worker threads we can have.  This is calculated when starting,
    /// and equals general_workers plus the sum of all categories' reserved threads counts plus the
    /// reserved batch workers count.  This is also used to signal a shutdown; we set it to 0 when
    /// quitting.
    int max_workers;

    /// Number of active workers
    int active_workers() const { return workers.size() - idle_workers.size(); }

    /// Worker thread loop
    void worker_thread(unsigned int index);

    /// If set, skip polling for one proxy loop iteration (set when we know we have something
    /// processible without having to shove it onto a socket, such as scheduling an internal job).
    bool proxy_skip_one_poll = false;

    /// Does the proxying work
    void proxy_loop();

    void proxy_conn_cleanup();

    void proxy_worker_message(std::vector<zmq::message_t>& parts);

    void proxy_process_queue();

    void proxy_schedule_reply_job(std::function<void()> f);

    /// Looks up a peers element given a connect index (for outgoing connections where we already
    /// knew the pubkey and SN status) or an incoming zmq message (which has the pubkey and sn
    /// status metadata set during initial connection authentication), creating a new peer element
    /// if required.
    decltype(peers)::iterator proxy_lookup_peer(int conn_index, zmq::message_t& msg);

    /// Handles built-in primitive commands in the proxy thread for things like "BYE" that have to
    /// be done in the proxy thread anyway (if we forwarded to a worker the worker would just have
    /// to send an instruction back to the proxy to do it).  Returns true if one was handled, false
    /// to continue with sending to a worker.
    bool proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts);

    struct run_info;
    /// Gets an idle worker's run_info and removes the worker from the idle worker list.  If there
    /// is no idle worker this creates a new `workers` element for a new worker (and so you should
    /// only call this if new workers are permitted).  Note that if this creates a new work info the
    /// worker will *not* yet be started, so the caller must create the thread (in `.thread`) after
    /// setting up the job if `.thread.joinable()` is false.
    run_info& get_idle_worker();

    /// Runs the worker; called after the `run` object has been set up.  If the worker thread hasn't
    /// been created then it is spawned; otherwise it is sent a RUN command.
    void proxy_run_worker(run_info& run);

    /// Sets up a job for a worker then signals the worker (or starts a worker thread)
    void proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts);

    /// proxy thread command handlers for commands sent from the outer object QUIT.  This doesn't
    /// get called immediately on a QUIT command: the QUIT commands tells workers to quit, then this
    /// gets called after all works have done so.
    void proxy_quit();

    // Sets the various properties for a listening socket prior to binding.  If curve is true then
    // the socket is set up using the keys and incoming connections must already know the pubkey to
    // establish a connection; otherwise the connection is plaintext without authentication.
    void setup_listening_socket(zmq::socket_t& socket, bool curve);

    // Sets the various properties on an outgoing socket prior to connection.  If remote_pubkey is
    // provided then the connection will be curve25519 encrypted and authenticate; otherwise it will
    // be unencrypted and unauthenticated.  Note that the remote end must be in the same mode (i.e.
    // either accepting curve connections, or not accepting curve).
    void setup_outgoing_socket(zmq::socket_t& socket, string_view remote_pubkey = {});

    /// Common connection implementation used by proxy_connect/proxy_send.  Returns the socket
    /// and, if a routing prefix is needed, the required prefix (or an empty string if not needed).
    /// For an optional connect that fail, returns nullptr for the socket.
    ///
    /// @param pubkey the pubkey to connect to
    /// @param connect_hint if we need a new connection and this is non-empty then we *may* use it
    /// instead of doing a call to `sn_lookup()`.
    /// @param optional if we don't already have a connection then don't establish a new one
    /// @param incoming_only only relay this if we have an established incoming connection from the
    /// given SN, otherwise don't connect (like `optional`)
    /// @param keep_alive the keep alive for the connection, if we establish a new outgoing
    /// connection.  If we already have an outgoing connection then its keep-alive gets increased to
    /// this if currently less than this.
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(string_view pubkey, string_view connect_hint,
            bool optional, bool incoming_only, std::chrono::milliseconds keep_alive);

    /// CONNECT_SN command telling us to connect to a new pubkey.  Returns the socket (which could
    /// be existing or a new one).  This basically just unpacks arguments and passes them on to
    /// proxy_connect_sn().
    std::pair<zmq::socket_t*, std::string> proxy_connect_sn(bt_dict_consumer data);

    /// Opens a new connection to a remote, with callbacks.  This is the proxy-side implementation
    /// of the `connect_remote()` call.
    void proxy_connect_remote(bt_dict_consumer data);

    /// Called to disconnect our remote connection to the given id (if we have one).
    void proxy_disconnect(bt_dict_consumer data);
    void proxy_disconnect(ConnectionID conn, std::chrono::milliseconds linger);

    /// SEND command.  Does a connect first, if necessary.
    void proxy_send(bt_dict_consumer data);

    /// REPLY command.  Like SEND, but only has a listening socket route to send back to and so is
    /// weaker (i.e. it cannot reconnect to the SN if the connection is no longer open).
    void proxy_reply(bt_dict_consumer data);

    /// Currently active batch/reply jobs; this is the container that owns the Batch instances
    std::unordered_set<detail::Batch*> batches;
    /// Individual batch jobs waiting to run
    using batch_job = std::pair<detail::Batch*, int>;
    std::queue<batch_job> batch_jobs, reply_jobs;
    int batch_jobs_active = 0;
    int reply_jobs_active = 0;
    int batch_jobs_reserved = -1;
    int reply_jobs_reserved = -1;
    /// Runs any queued batch jobs
    void proxy_run_batch_jobs(std::queue<batch_job>& jobs, int reserved, int& active, bool reply);

    /// BATCH command.  Called with a Batch<R> (see arqmamq/batch.h) object pointer for the proxy to
    /// take over and queue batch jobs.
    void proxy_batch(detail::Batch* batch);

    /// TIMER command.  Called with a serialized list containing: function pointer to assume
    /// ownership of, an interval count (in ms), and whether or not jobs should be squelched (see
    /// `add_timer()`).
    void proxy_timer(bt_list_consumer timer_data);

    /// Same, but deserialized
    void proxy_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch);

    /// ZAP (https://rfc.zeromq.org/spec:27/ZAP/) authentication handler; this does non-blocking
    /// processing of any waiting authentication requests for new incoming connections.
    void process_zap_requests();

    /// Handles a control message from some outer thread to the proxy
    void proxy_control_message(std::vector<zmq::message_t>& parts);

    /// Closing any idle connections that have outlived their idle time.  Note that this only
    /// affects outgoing connections; incomings connections are the responsibility of the other end.
    void proxy_expire_idle_peers();

    /// Helper method to actually close a remote connection and update the stuff that needs updating.
    void proxy_close_connection(size_t removed, std::chrono::milliseconds linger);

    /// Closes an outgoing connection immediately, updates internal variables appropriately.
    /// Returns the next iterator (the original may or may not be removed from peers, depending on
    /// whether or not it also has an active incoming connection).
    decltype(peers)::iterator proxy_close_outgoing(decltype(peers)::iterator it);

    struct category {
        Access access;
        std::unordered_map<std::string, std::pair<CommandCallback, bool /*is_request*/>> commands;
        unsigned int reserved_threads = 0;
        unsigned int active_threads = 0;
        int max_queue = 200;
        int queued = 0;

        category(Access access, unsigned int reserved_threads, int max_queue)
            : access{access}, reserved_threads{reserved_threads}, max_queue{max_queue} {}
    };

    /// Categories, mapped by category name.
    std::unordered_map<std::string, category> categories;

    /// For enabling backwards compatibility with command renaming: this allows mapping one command
    /// to another in a different category (which happens before the category and command lookup is
    /// done).
    std::unordered_map<std::string, std::string> command_aliases;

    /// Retrieve category and callback from a command name, including alias mapping.  Warns on
    /// invalid commands and returns nullptrs.  The command name will be updated in place if it is
    /// aliased to another command.
    std::pair<category*, const std::pair<CommandCallback, bool>*> get_command(std::string& command);

    /// Checks a peer's authentication level.  Returns true if allowed, warns and returns false if
    /// not.
    bool proxy_check_auth(size_t conn_index, bool outgoing, const peer_info& peer,
            const std::string& command, const category& cat, zmq::message_t& msg);

    /// Details for a pending command; such a command already has authenticated access and is just
    /// waiting for a thread to become available to handle it.
    struct pending_command {
        category& cat;
        std::string command;
        std::vector<zmq::message_t> data_parts;
        const std::pair<CommandCallback, bool>* callback;
        ConnectionID conn;

        pending_command(category& cat, std::string command, std::vector<zmq::message_t> data_parts,
                const std::pair<CommandCallback, bool>* callback, ConnectionID conn)
            : cat{cat}, command{std::move(command)}, data_parts{std::move(data_parts)},
            callback{callback}, conn{std::move(conn)} {}
    };
    std::list<pending_command> pending_commands;


    /// End of proxy-specific members
    ///////////////////////////////////////////////////////////////////////////////////


    /// Structure that contains the data for a worker thread - both the thread itself, plus any
    /// transient data we are passing into the thread.
    struct run_info {
        bool is_batch_job = false;
        bool is_reply_job = false;

        // If is_batch_job is false then these will be set appropriate (if is_batch_job is true then
        // these shouldn't be accessed and likely contain stale data).
        category *cat;
        std::string command;
        ConnectionID conn; // The connection (or SN pubkey) to reply on/to.
        std::string conn_route; // if non-empty this is the reply routing prefix (for incoming connections)
        std::vector<zmq::message_t> data_parts;

        // If is_batch_job true then these are set (if is_batch_job false then don't access these!):
        int batch_jobno; // >= 0 for a job, -1 for the completion job

        union {
            const std::pair<CommandCallback, bool>* callback; // set if !is_batch_job
            detail::Batch* batch;                             // set if is_batch_job
        };

        // These belong to the proxy thread and must not be accessed by a worker:
        std::thread worker_thread;
        size_t worker_id; // The index in `workers`
        std::string worker_routing_id; // "w123" where 123 == worker_id

        /// Loads the run info with an incoming command
        run_info& load(category* cat, std::string command, ConnectionID conn,
                std::vector<zmq::message_t> data_parts, const std::pair<CommandCallback, bool>* callback);

        /// Loads the run info with a stored pending command
        run_info& load(pending_command&& pending);
        /// Loads the run info with a batch job
        run_info& load(batch_job&& bj, bool reply_job = false);
    };
    /// Data passed to workers for the RUN command.  The proxy thread sets elements in this before
    /// sending RUN to a worker then the worker uses it to get call info, and only allocates it
    /// once, before starting any workers.  Workers may only access their own index and may not
    /// change it.
    std::vector<run_info> workers;

public:
    /**
     * ArqmaMQ constructor.  This constructs the object but does not start it; you will typically
     * want to first add categories and commands, then finish startup by invoking `start()`.
     * (Categories and commands cannot be added after startup).
     *
     * @param pubkey the public key (32-byte binary string).  For a service node this is the service
     * node x25519 keypair.  For non-service nodes this (and privkey) can be empty strings to
     * automatically generate an ephemeral keypair.
     *
     * @param privkey the service node's private key (32-byte binary string), or empty to generate
     * one.
     *
     * @param service_node - true if this instance should be considered a service node for the
     * purpose of allowing "Access::local_sn" remote calls.  (This should be true if we are
     * *capable* of being a service node, whether or not we are currently actively).  If specified
     * as true then the pubkey and privkey values must not be empty.
     *
     * @param sn_lookup function that takes a pubkey key (32-byte binary string) and returns a
     * connection string such as "tcp://1.2.3.4:23456" to which a connection should be established
     * to reach that service node.  Note that this function is only called if there is no existing
     * connection to that service node, and that the function is never called for a connection to
     * self (that uses an internal connection instead).  Also note that the service node must be
     * listening in curve25519 mode (otherwise we couldn't verify its authenticity).  Should return
     * empty for not found or if SN lookups are not supported.
     *
     * @param allow_incoming is a callback that ArqmaMQ can use to determine whether an incoming
     * connection should be allowed at all and, if so, whether the connection is from a known
     * service node.  Called with the connecting IP, the remote's verified x25519 pubkey, and the 
     * called on incoming connections with the (verified) incoming connection
     * pubkey (32-byte binary string) to determine whether the given SN should be allowed to
     * connect.
     *
     * @param log a function or callable object that writes a log message.  If omitted then all log
     * messages are suppressed.
     */
    ArqmaMQ( std::string pubkey,
            std::string privkey,
            bool service_node,
            SNRemoteAddress sn_lookup,
            Logger logger = [](LogLevel, const char*, int, std::string) { });

    /**
     * Simplified ArqmaMQ constructor for a simple listener without any SN connection/authentication
     * capabilities.  This treats all remotes as "basic", non-service node connections for command
     * authentication purposes.
     */
    explicit ArqmaMQ(Logger logger = [](LogLevel, const char*, int, std::string) { })
        : ArqmaMQ("", "", false, [](auto) { return ""s; /*no peer lookups*/ }, std::move(logger)) {}

    /**
     * Destructor; instructs the proxy to quit.  The proxy tells all workers to quit, waits for them
     * to quit and rejoins the threads then quits itself.  The outer thread (where the destructor is
     * running) rejoins the proxy thread.
     */
    ~ArqmaMQ();

    /// Sets the log level of the ArqmaMQ object.
    void log_level(LogLevel level);

    /// Gets the log level of the ArqmaMQ object.
    LogLevel log_level() const;

    /**
     * Add a new command category.  This method may not be invoked after `start()` has been called.
     * This method is also not thread safe, and is generally intended to be called (along with
     * add_command) immediately after construction and immediately before calling start().
     *
     * @param name - the category name which must consist of one or more characters and may not
     * contain a ".".
     *
     * @param access_level the access requirements for remote invocation of the commands inside this
     * category.
     *
     * @param reserved_threads if non-zero then the worker thread pool will ensure there are at at
     * least this many threads either current processing or available to process commands in this
     * category.  This is used to ensure that a category's commands can be invoked even if
     * long-running commands in some other category are currently using all worker threads.  This
     * can increase the number of worker threads above the `general_workers` parameter given in the
     * constructor, but will only do so if the need arised: that is, if a command request arrives
     * for a category when all workers are busy and no worker is currently processing any command in
     * that category.
     *
     * @param max_queue is the maximum number of incoming messages in this category that we will
     * queue up when waiting for a worker to become available for this category.  Once the queue for
     * a category exceeds this many incoming messages then new messages will be dropped until some
     * messages are processed off the queue.  -1 means unlimited, 0 means we will never queue (which
     * means just dropping messages for this category if no workers are available to instantly
     * handle the request).
     *
     * @returns a CatHelper object that makes adding commands slightly less verbose (see the
     * CatHelper describe, above).
     */
    CatHelper add_category(std::string name, Access access_level, unsigned int reserved_threads = 0, int max_queue = 200);

    /**
     * Adds a new command to an existing category.  This method may not be invoked after `start()`
     * has been called.
     *
     * @param category - the category name (must already be created by a call to `add_category`)
     *
     * @param name - the command name, without the `category.` prefix.
     *
     * @param callback - a callable object which is callable as `callback(zeromq::Message &)`
     */
    void add_command(const std::string& category, std::string name, CommandCallback callback);

    /**
     * Adds a new "request" command to an existing category.  These commands are just like normal
     * commands, but are expected to call `msg.send_reply()` with any data parts on every request,
     * while normal commands are more general.
     *
     * Parameters given here are identical to `add_command()`.
     */
    void add_request_command(const std::string& category, std::string name, CommandCallback callback);

    /**
     * Adds a command alias; this is intended for temporary backwards compatibility: if any aliases
     * are defined then every command (not just aliased ones) has to be checked on invocation to see
     * if it is defined in the alias list.  May not be invoked after `start()`.
     *
     * Aliases should follow the `category.command` format for both the from and to names, and
     * should only be called for `to` categories that are already defined.  The category name is not
     * currently enforced on the `from` name (for backwards compatility with Loki's quorumnet code)
     * but will be at some point.
     *
     * Access permissions for an aliased command depend only on the mapped-to value; for example, if
     * `cat.meow` is aliased to `dog.bark` then it is the access permissions on `dog` that apply,
     * not those of `cat`, even if `cat` is more restrictive than `dog`.
     */
    void add_command_alias(std::string from, std::string to);

    /**
     * Sets the number of worker threads reserved for batch jobs.  If not explicitly called then
     * this defaults to half the general worker threads configured (rounded up).  This works exactly
     * like reserved_threads for a category, but allows to batch jobs.  See category for details.
     *
     * Note that some internal jobs are counted as batch jobs: in particular timers added via
     * add_timer() are scheduled as batch jobs.
     *
     * Cannot be called after start()ing the ArqmaMQ instance.
     */
    void set_batch_threads(int threads);

    /**
     * Sets the number of worker threads reserved for handling replies from servers; this is
     * mostly for responses to `request()` calls, but also gets used for other network-related
     * events such as the ConnectSuccess/ConnectFailure callbacks for establishing remote non-SN
     * connections.
     *
     * Defaults to one-eighth of the number of configured general threads, rounded up.
     *
     * Cannot be changed after start()ing the ArqmaMQ instance.
     */
    void set_reply_threads(int threads);

    /**
     * Sets the number of general worker threads.  This is the target number of threads to run that
     * we generally try not to exceed.  These threads can be used for any command, and will be
     * created (up to the limit) on demand.  Note that individual categories (or batch jobs) with
     * reserved threads can create threads in addition to the amount specified here if necessary to
     * fulfill the reserved threads count for the category.
     *
     * Adjusting this also adjusts the default values of batch and reply threads, above.
     *
     * Defaults to `std::thread::hardware_concurrency()`.
     *
     * Cannot be called after start()ing the ArqmaMQ instance.
     */
    void set_general_threads(int threads);

    /**
     * Finish starting up: binds to the bind locations given in the constructor and launches the
     * proxy thread to handle message dispatching between remote nodes and worker threads.
     *
     * You will need to call `add_category` and `add_command` to register commands before calling
     * `start()`; once start() is called commands cannot be changed.
     */
    void start();

    /** Start listening on the given bind address using curve authentication/encryption.  Incoming
     * connections will only be allowed from clients that already have the server's pubkey, and
     * will be encrypted.  `allow_connection` is invoked for any incoming connections on this
     * address to determine the incoming remote's access and authentication level.
     *
     * @param bind address - can be any string zmq supports; typically a tcp IP/port combination
     * such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".
     *
     * @param allow_connection function to call to determine whether to allow the connection and, if
     * so, the authentication level it receives.  If omitted the default returns non-service node,
     * AuthLevel::none access.
     */
    void listen_curve(std::string bind, AllowFunc allow_connection = [](auto, auto) { return Allow{AuthLevel::none, false}; });

    /** Start listening on the given bind address in unauthenticated plain text mode.  Incoming
     * connections can come from anywhere.  `allow_connection` is invoked for any incoming
     * connections on this address to determine the incoming remote's access and authentication
     * level.  Note that `allow_connection` here will be called with an empty pubkey.
     *
     * @param bind address - can be any string zmq supports; typically a tcp IP/port combination
     * such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".
     *
     * @param allow_connection function to call to determine whether to allow the connection and, if
     * so, the authentication level it receives.  If omitted the default returns non-service node,
     * AuthLevel::none access.
     */
    void listen_plain(std::string bind, AllowFunc allow_connection = [](auto, auto) { return Allow{AuthLevel::none, false}; });

    /**
     * Try to initiate a connection to the given SN in anticipation of needing a connection in the
     * future.  If a connection is already established, the connection's idle timer will be reset
     * (so that the connection will not be closed too soon).  If the given idle timeout is greater
     * than the current idle timeout then the timeout increases to the new value; if less than the
     * current timeout it is ignored.  (Note that idle timeouts only apply if the existing
     * connection is an outgoing connection).
     *
     * Note that this method (along with send) doesn't block waiting for a connection; it merely
     * instructs the proxy thread that it should establish a connection.
     *
     * @param pubkey - the public key (32-byte binary string) of the service node to connect to
     * @param keep_alive - the connection will be kept alive if there was valid activity within
     *                     the past `keep_alive` milliseconds.  If an outgoing connection already
     *                     exists, the longer of the existing and the given keep alive is used.
     *                     (Note that the default applied here is much longer than the default for an
     *                     implicit connect() by calling send() directly.)
     * @param hint - if non-empty and a new outgoing connection needs to be made this hint value
     *               may be used instead of calling the lookup function.  (Note that there is no
     *               guarantee that the hint will be used; it is only usefully specified if the
     *               connection address has already been incidentally determined).
     *
     * @returns a ConnectionID that identifies an connection with the given SN.  Typically you
     * *don't* need to worry about this (and can just discard it): you can always simply pass the
     * pubkey as a string wherever a ConnectionID is called.
     */
    ConnectionID connect_sn(string_view pubkey, std::chrono::milliseconds keep_alive = 5min, string_view hint = {});

    /**
     * Establish a connection to the given remote with callbacks invoked on a successful or failed
     * connection.  Returns a ConnectionID associated with the connection being attempted.  It is
     * possible to send to the remote before the successful callback is invoked, but there is no
     * guarantee that the messages will be delivered (e.g. if the connection ultimately fails).
     *
     * For connections to a service node you generally want connect_sn() instead (which verifies
     * that it is talking to the SN and encrypts the connection).
     *
     * Unlike `connect_sn`, the connection established here will be kept open indefinitely (until
     * you call disconnect).
     *
     * The `on_connect` and `on_failure` callbacks are invoked when a connection has been
     * established or failed to establish.
     *
     * @param remote the remote connection address, such as `tcp://localhost:1234`.
     * @param on_connect called with the identifier after the connection has been established.
     * @param on_failure called with the identifier and failure message if we fail to connect.
     * @param pubkey if non-empty then connect securely (using curve encryption) and verify that the
     * remote's pubkey equals the given value.  Specifying this is similar to using connect_sn()
     * except that we do not treat the remote as a SN for command authorization purposes.
     * @param auth_level determines the authentication level of the remote for issuing commands to
     * us.  The default is `AuthLevel::none`.
     * @param timeout how long to try before aborting the connection attempt and calling the
     * on_failure callback.  Note that the connection can fail for various reasons before the
     * timeout.
     *
     * @param returns ConnectionID that uniquely identifies the connection to this remote node.  In
     * order to talk to it you will need the returned value (or a copy of it).
     */
    ConnectionID connect_remote(string_view remote, ConnectSuccess on_connect, ConnectFailure on_failure,
            string_view pubkey = {},
            AuthLevel auth_level = AuthLevel::none,
            std::chrono::milliseconds timeout = REMOTE_CONNECT_TIMEOUT);

    /**
     * Disconnects an established outgoing connection established with `connect_remote()` (or, less
     * commonly, `connect_sn()`).
     *
     * @param id the connection id, as returned by `connect_remote()` or the SN pubkey.
     *
     * @param linger how long to allow the connection to linger while there are still pending
     * outbound messages to it before disconnecting and dropping any pending messages.  (Note that
     * this lingering is internal; the disconnect_remote() call does not block).  The default is 1
     * second.
     *
     * If given a pubkey, we try to close an outgoing connection to the given SN if one exists; note
     * however that this is often not particularly useful as messages to that SN can immediately
     * reopen the connection.
     */
    void disconnect(ConnectionID id, std::chrono::milliseconds linger = 1s);

    /**
     * Queue a message to be relayed to the given service node or remote without requiring a reply.
     * ArqmaMQ will attempt to relay the message (first connecting and handshaking to the remote SN
     * if not already connected).
     *
     * If a new connection is established it will have a relatively short (30s) idle timeout.  If
     * the connection should stay open longer you should either call `connect(pubkey, IDLETIME)` or
     * pass a a `send_option::keep_alive{IDLETIME}` in `opts`.
     *
     * Note that this method (along with connect) doesn't block waiting for a connection or for the
     * message to send; it merely instructs the proxy thread that it should send.  ZMQ will
     * generally try hard to deliver it (reconnecting if the connection fails), but if the
     * connection fails persistently the message will eventually be dropped.
     *
     * @param remote - either a ConnectionID value returned by connect_remote, or a service node
     *                 pubkey string.  In the latter case, sending the message may trigger a new
     *                 connection being established to the service node (i.e. you do not have to
     *                 call connect() first).
     * @param cmd - the first data frame value which is almost always the remote "category.command" name
     * @param opts - any number of std::string (or string_views) and send options.  Each send option
     *               affects how the send works; each string becomes a message part.
     *
     * Example:
     *
     *     // Send to a SN, connecting to it if we aren't already connected:
     *     arqmq.send(pubkey, "hello.world", "abc", send_option::hint("tcp://localhost:1234"), "def");
     *
     *     // Start connecting to a remote and immediately queue a message for it
     *     auto conn = arqmq.connect_remote("tcp://127.0.0.1:1234",
     *         [](ConnectionID) { std::cout << "connected\n"; },
     *         [](ConnectionID, string_view why) { std::cout << "connection failed: " << why << \n"; });
     *     arqmq.send(conn, "hello.world", "abc", "def");
     *
     * Both of these send the command `hello.world` to the given pubkey, containing additional
     * message parts "abc" and "def".  In the first case, if not currently connected, the given
     * connection hint may be used rather than performing a connection address lookup on the pubkey.
     */
    template <typename... T>
    void send(ConnectionID to, string_view cmd, const T&... opts);

    /** Send a command configured as a "REQUEST" command to a service node: the data parts will be
     * prefixed with a random identifier.  The remote is expected to reply with a ["REPLY",
     * <identifier>, ...] message, at which point we invoke the given callback with any [...] parts
     * of the reply.
     *
     * Like `send()`, a new connection to the service node will be established if not already
     * connected.
     *
     * @param to - the pubkey string or ConnectionID to send this request to
     * @param cmd - the command name
     * @param callback - the callback to invoke when we get a reply.  Called with a true value and
     * the data strings when a reply is received, or false and an empty vector of data parts if we
     * get no reply in the timeout interval.
     * @param opts - anything else (i.e. strings, send_options) is forwarded to send().
     */
    template <typename... T>
    void request(ConnectionID to, string_view cmd, ReplyCallback callback, const T&... opts);

    /// The key pair this ArqmaMQ was created with; if empty keys were given during construction then
    /// this returns the generated keys.
    const std::string& get_pubkey() const { return pubkey; }
    const std::string& get_privkey() const { return privkey; }

    /**
     * Batches a set of jobs to be executed by workers, optionally followed by a completion function.
     *
     * Must include arqmamq/batch.h to use.
     */
    template <typename R>
    void batch(Batch<R>&& batch);

    /**
     * Queues a single job to be executed with no return value.  This is a shortcut for creating and
     * submitting a single-job, no-completion-function batch job.
     */
    void job(std::function<void()> f);

    /**
     * Adds a timer that gets scheduled periodically in the job queue.  Normally jobs are not
     * double-booked: that is, a new timed job will not be scheduled if the timer fires before a
     * previously scheduled callback of the job has not yet completed.  If you want to override this
     * (so that, under heavy load or long jobs, there can be more than one of the same job scheduled
     * or running at a time) then specify `squelch` as `false`.
     */
    void add_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch = true);
};

/// Helper class that slightly simplifies adding commands to a category.
///
/// This allows simplifying:
///
/// arqmq.add_category("foo", ...);
/// arqmq.add_command("foo", "a", ...);
/// arqmq.add_command("foo", "b", ...);
/// arqmq.add_request_command("foo", "c", ...);
///
/// to:
///
/// arqmq.add_category("foo", ...)
///     .add_command("a", ...)
///     .add_command("b", ...)
///     .add_request_command("b", ...)
///     ;
class CatHelper {
    ArqmaMQ& arqmq;
    std::string cat;

public:
    CatHelper(ArqmaMQ& arqmq, std::string cat) : arqmq{arqmq}, cat{std::move(cat)} {}

    CatHelper& add_command(std::string name, ArqmaMQ::CommandCallback callback) {
        arqmq.add_command(cat, std::move(name), std::move(callback));
        return *this;
    }

    CatHelper& add_request_command(std::string name, ArqmaMQ::CommandCallback callback) {
        arqmq.add_request_command(cat, std::move(name), std::move(callback));
        return *this;
    }
};


/// Namespace for options to the send() method
namespace send_option {

template <typename InputIt>
struct data_parts_impl {
    InputIt begin, end;
    data_parts_impl(InputIt begin, InputIt end) : begin{std::move(begin)}, end{std::move(end)} {}
};

/// Specifies an iterator pair of data options to send, for when the number of arguments to send()
/// cannot be determined at compile time.
template <typename InputIt>
data_parts_impl<InputIt> data_parts(InputIt begin, InputIt end) { return {std::move(begin), std::move(end)}; }

/// Specifies a connection hint when passed in to send().  If there is no current connection to the
/// peer then the hint is used to save a call to the SNRemoteAddress to get the connection location.
/// (Note that there is no guarantee that the given hint will be used or that a SNRemoteAddress call
/// will not also be done.)
struct hint {
    std::string connect_hint;
    // Constructor taking a hint.  If the hint is an empty string then no hint will be used.
    explicit hint(std::string connect_hint) : connect_hint{std::move(connect_hint)} {}
};

/// Does a send() if we already have a connection (incoming or outgoing) with the given peer,
/// otherwise drops the message.
struct optional {
    bool is_optional = true;
    // Constructor; default construction gives you an optional, but the bool parameter can be
    // specified as false to explicitly make a connection non-optional instead.
    explicit optional(bool opt = true) : is_optional{opt} {}
};

/// Specifies that the message should be sent only if it can be sent on an existing incoming socket,
/// and dropped otherwise.
struct incoming {
    bool is_incoming = true;
    // Constructor; default construction gives you an incoming-only, but the bool parameter can be
    // specified as false to explicitly disable incoming-only behaviour.
    explicit incoming(bool inc = true) : is_incoming{inc} {}
};

/// Specifies the idle timeout for the connection - if a new or existing outgoing connection is used
/// for the send and its current idle timeout setting is less than this value then it is updated.
struct keep_alive {
    std::chrono::milliseconds time;
    explicit keep_alive(std::chrono::milliseconds time) : time{std::move(time)} {}
};

}

namespace detail {

// Sends a control message to the given socket consisting of the command plus optional dict
// data (only sent if the data is non-empty).
void send_control(zmq::socket_t& sock, string_view cmd, std::string data = {});

/// Base case: takes a string-like value and appends it to the message parts
inline void apply_send_option(bt_list& parts, bt_dict&, string_view arg) {
    parts.emplace_back(arg);
}

/// `data_parts` specialization: appends a range of serialized data parts to the parts to send
template <typename InputIt>
void apply_send_option(bt_list& parts, bt_dict&, const send_option::data_parts_impl<InputIt> data) {
    for (auto it = data.begin; it != data.end; ++it)
        parts.push_back(arqmamq::bt_deserialize(*it));
}

/// `hint` specialization: sets the hint in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::hint& hint) {
    control_data["hint"] = hint.connect_hint;
}

/// `optional` specialization: sets the optional flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::optional& o) {
    control_data["optional"] = o.is_optional;
}

/// `incoming` specialization: sets the incoming-only flag in the control data
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::incoming& i) {
    control_data["incoming"] = i.is_incoming;
}

/// `keep_alive` specialization: increases the outgoing socket idle timeout (if shorter)
inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::keep_alive& timeout) {
    control_data["keep-alive"] = timeout.time.count();
}

} // namespace detail

template <typename... T>
bt_dict build_send(ConnectionID to, string_view cmd, const T&... opts) {
    bt_dict control_data;
    bt_list parts{{cmd}};
#ifdef __cpp_fold_expressions
    (detail::apply_send_option(parts, control_data, opts),...);
#else
    (void) std::initializer_list<int>{(detail::apply_send_option(parts, control_data, opts), 0)...};
#endif

    if (to.sn())
        control_data["conn_pubkey"] = std::move(to.pk);
    else {
        control_data["conn_id"] = to.id;
        control_data["conn_route"] = std::move(to.route);
    }
    control_data["send"] = std::move(parts);
    return control_data;

}

template <typename... T>
void ArqmaMQ::send(ConnectionID to, string_view cmd, const T&... opts) {
    detail::send_control(get_control_socket(), "SEND",
            bt_serialize(build_send(std::move(to), cmd, opts...)));
}

std::string make_random_string(size_t size);

template <typename... T>
void ArqmaMQ::request(ConnectionID to, string_view cmd, ReplyCallback callback, const T &...opts) {
    const auto reply_tag = make_random_string(15); // 15 random bytes is lots and should keep us in most stl implementations' small string optimization
    bt_dict control_data = build_send(std::move(to), cmd, reply_tag, opts...);
    control_data["request"] = true;
    control_data["request_callback"] = reinterpret_cast<uintptr_t>(new ReplyCallback{std::move(callback)});
    control_data["request_tag"] = string_view{reply_tag};
    detail::send_control(get_control_socket(), "SEND", bt_serialize(std::move(control_data)));
}

template <typename... Args>
void Message::send_back(string_view command, Args&&... args) {
    arqmamq.send(conn, command, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

template <typename... Args>
void Message::send_reply(Args&&... args) {
    assert(!reply_tag.empty());
    arqmamq.send(conn, "REPLY", reply_tag, send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

template <typename ReplyCallback, typename... Args>
void Message::send_request(string_view cmd, ReplyCallback&& callback, Args&&... args) {
    arqmamq.request(conn, cmd, std::forward<ReplyCallback>(callback),
            send_option::optional{!conn.sn()}, std::forward<Args>(args)...);
}

template <typename... T>
void ArqmaMQ::log_(LogLevel lvl, const char* file, int line, const T&... stuff) {
    if (log_level() < lvl)
        return;

    std::ostringstream os;
#ifdef __cpp_fold_expressions
    (os << ... << stuff);
#else
    (void) std::initializer_list<int>{(os << stuff, 0)...};
#endif
    logger(lvl, file, line, os.str());
}

std::ostream &operator<<(std::ostream &os, LogLevel lvl);

} // namespace arqmamq

// vim:sw=4:et
