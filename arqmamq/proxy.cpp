#include "arqmamq.h"
#include "arqmamq-internal.h"
#include "hex.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD_)
extern "C" {
#include <pthread.h>
#include <pthread_np.h>
}
#endif

#ifndef _WIN32
extern "C" {
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}
#endif

namespace arqmamq {

void ArqmaMQ::proxy_quit() {
    AMQ_LOG(debug, "Received quit command, shutting down proxy thread");

    assert(std::none_of(workers.begin(), workers.end(), [](auto& worker) { return worker.worker_thread.joinable(); }));
    assert(std::none_of(tagged_workers.begin(), tagged_workers.end(), [](auto& worker) { return std::get<0>(worker).worker_thread.joinable(); }));

    command.set(zmq::sockopt::linger, 0);
    command.close();
    {
        std::lock_guard lock{control_sockets_mutex};
        proxy_shutting_down = true; // To prevent threads from opening new control sockets
    }
    workers_socket.close();
    int linger = std::chrono::milliseconds{CLOSE_LINGER}.count();
    for (auto& s : connections)
        s.set(zmq::sockopt::linger, linger);
    connections.clear();
    peers.clear();

    AMQ_LOG(debug, "Proxy thread teardown complete");
}

void ArqmaMQ::proxy_send(bt_dict_consumer data) {

    // NB: bt_dict_consumer goes in alphabetical order
    std::string_view hint;
    std::chrono::milliseconds keep_alive{DEFAULT_SEND_KEEP_ALIVE};
    std::chrono::milliseconds request_timeout{DEFAULT_REQUEST_TIMEOUT};
    bool optional = false;
    bool outgoing = false;
    bool incoming = false;
    bool request = false;
    bool have_conn_id = false;
    ConnectionID conn_id;

    std::string request_tag;
    ReplyCallback request_callback;
    if (data.skip_until("conn_id")) {
        conn_id.id = data.consume_integer<long long>();
        if (conn_id.id == -1)
            throw std::runtime_error("Invalid error: invalid conn_id value (-1)");
        have_conn_id = true;
    }
    if (data.skip_until("conn_pubkey")) {
        if (have_conn_id)
            throw std::runtime_error("Internal error: Invalid proxy send command; conn_id and conn_pubkey are exclusive");
        conn_id.pk = data.consume_string();
        conn_id.id = ConnectionID::SN_ID;
    } else if (!have_conn_id)
        throw std::runtime_error("Internal error: Invalid proxy send command; conn_pubkey or conn_id missing");
    if (data.skip_until("conn_route"))
        conn_id.route = data.consume_string();
    if (data.skip_until("hint"))
        hint = data.consume_string_view();
    if (data.skip_until("incoming"))
        incoming = data.consume_integer<bool>();
    if (data.skip_until("keep_alive"))
        keep_alive = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    if (data.skip_until("optional"))
        optional = data.consume_integer<bool>();
    if (data.skip_until("outgoing"))
        outgoing = data.consume_integer<bool>();

    if (data.skip_until("request"))
        request = data.consume_integer<bool>();
    if (request) {
        if (!data.skip_until("request_callback"))
            throw std::runtime_error("Internal error: received request without request_callback");
        request_callback = detail::deserialize_object<ReplyCallback>(data.consume_integer<uintptr_t>());

        if (!data.skip_until("request_tag"))
            throw std::runtime_error("Internal error: received request without request_name");
        request_tag = data.consume_string();
        if (data.skip_until("request_timeout"))
          request_timeout = std::chrono::milliseconds{data.consume_integer<uint64_t>()};
    }
    if (!data.skip_until("send"))
        throw std::runtime_error("Internal error: Invalid proxy send command; send parts missing");
    bt_list_consumer send = data.consume_list_consumer();

    send_option::queue_failure::callback_t callback_nosend;
    if (data.skip_until("send_fail"))
      callback_nosend = detail::deserialize_object<decltype(callback_nosend)>(data.consume_integer<uintptr_t>());

    send_option::queue_full::callback_t callback_noqueue;
    if (data.skip_until("send_full_q"))
      callback_noqueue = detail::deserialize_object<decltype(callback_noqueue)>(data.consume_integer<uintptr_t>());

    bool retry = true, sent = false, nowarn = false;
    while (retry) {
        retry = false;
        zmq::socket_t *send_to;
        if (conn_id.sn()) {
            auto sock_route = proxy_connect_sn(conn_id.pk, hint, optional, incoming, outgoing, EPHEMERAL_ROUTING_ID, keep_alive);
            if (!sock_route.first) {
                nowarn = true;
                if (optional)
                  AMQ_LOG(debug, "Not sending: send is optionao and no connection to ", to_hex(conn_id.pk), " is currently established");
                else
                  AMQ_LOG(error, "Unable to send to ", to_hex(conn_id.pk), ": no valid connection address found");
                break;
            }
            send_to = sock_route.first;
            conn_id.route = std::move(sock_route.second);
        } else if (!conn_id.route.empty()) {
            auto it = incoming_conn_index.find(conn_id.unrouted());
            if (it == incoming_conn_index.end()) {
                AMQ_LOG(warn, "Unable to send to ", conn_id, ": incoming listening socket not found");
                break;
            }
            send_to = &connections[it->second];
        } else {
            auto pr = peers.equal_range(conn_id);
            if (pr.first == peers.end()) {
                AMQ_LOG(warn, "Unable to send: connection id ", conn_id, " is not (or is no longer) a valid outgoing connection");
                break;
            }
            auto& peer = pr.first->second;
            send_to = &connections[peer.conn_index];
        }

        try {
            sent = send_message_parts(*send_to, build_send_parts(send, conn_id.route));
        } catch (const zmq::error_t &e) {
            if (e.num() == EHOSTUNREACH && !conn_id.route.empty()) {
                AMQ_LOG(debug, "Incoming connection is no longer valid. Removing peer details");

                auto pr = peers.equal_range(conn_id);
                if (pr.first != peers.end()) {
                    if (!conn_id.sn()) {
                        peers.erase(pr.first);
                    } else {
                        bool removed;
                        for (auto it = pr.first; it != pr.second; ) {
                            auto& peer = it->second;
                            if (peer.route == conn_id.route) {
                                peers.erase(it);
                                removed = true;
                                break;
                            }
                        }
                        if (removed) {
                            AMQ_LOG(debug, "Retrying sending to SN ", to_hex(conn_id.pk), " using other sockets");
                            retry = true;
                        }
                    }
                }
            }
            if (!retry) {
                if (!conn_id.sn() && !conn_id.route.empty())
                {
                  AMQ_LOG(debug, "Unable to send message to incoming connection ", conn_id, ": ", e.what(), "; remote has probably disconnected");
                }
                else
                {
                  AMQ_LOG(warn, "Unable to send message to ", conn_id, ": ", e.what());
                }
                nowarn = true;
                if (callback_nosend) {
                  job([callback = std::move(callback_nosend), error = e] { callback(&error); });
                  callback_nosend = nullptr;
                }
            }
        }
    }
    if (request) {
        if (sent) {
            AMQ_LOG(debug, "Added new pending request ", to_hex(request_tag));
            pending_requests.insert({ request_tag, {std::chrono::steady_clock::now() + request_timeout, std::move(request_callback) }});
        } else {
            AMQ_LOG(debug, "Could not send request, scheduling request callback failure");
            job([callback = std::move(request_callback)] { callback(false, {{"TIMEOUT"s}}); });
        }
    }
    if (!sent) {
      if (callback_nosend)
        job([callback = std::move(callback_nosend)] { callback(nullptr); });
      else if (callback_noqueue)
        job(std::move(callback_noqueue));
      else if (!nowarn)
        AMQ_LOG(warn, "Unable to send message to ", conn_id, ": sending would block");
    }
}

void ArqmaMQ::proxy_reply(bt_dict_consumer data) {
    bool have_conn_id = false;
    ConnectionID conn_id{0};
    if (data.skip_until("conn_id")) {
        conn_id.id = data.consume_integer<long long>();
        if (conn_id.id == -1)
            throw std::runtime_error("Invalid error: invalid conn_id value (-1)");
        have_conn_id = true;
    }
    if (data.skip_until("conn_pubkey")) {
        if (have_conn_id)
            throw std::runtime_error("Internal error: Invalid proxy reply command; conn_id and conn_pubkey are exclusive");
        conn_id.pk = data.consume_string();
        conn_id.id = ConnectionID::SN_ID;
    } else if (!have_conn_id)
        throw std::runtime_error("Internal error: Invalid proxy reply command; conn_pubkey or conn_id missing");
    if (!data.skip_until("send"))
        throw std::runtime_error("Internal error: Invalid proxy reply command; send parts missing");

    bt_list_consumer send = data.consume_list_consumer();

    auto pr = peers.equal_range(conn_id);
    if (pr.first == pr.second) {
        AMQ_LOG(warn, "Unable to send tagged reply: the connection is no longer valid");
        return;
    }

    // We try any connections until one works (for ordinary remotes there will be just one, but for
    // SNs there might be one incoming and one outgoing).
    for (auto it = pr.first; it != pr.second; ) {
        try {
            send_message_parts(connections[it->second.conn_index], build_send_parts(send, it->second.route));
            break;
        } catch (const zmq::error_t &err) {
            if (err.num() == EHOSTUNREACH) {
                AMQ_LOG(debug, "Unable to send reply to incoming non-SN request: remote is no longer connected. Removing peer details");
                it = peers.erase(it);
            } else {
                AMQ_LOG(warn, "Unable to send reply to incoming non-SN request: ", err.what());
                ++it;
            }
        }
    }
}

void ArqmaMQ::proxy_control_message(std::vector<zmq::message_t>& parts) {
    if (parts.size() < 2)
        throw std::logic_error("ArqmaMQ bug: Expected 2-3 message parts for a proxy control message");
    auto route = view(parts[0]), cmd = view(parts[1]);
    AMQ_TRACE("control message: ", cmd);
    if (parts.size() == 3) {
        AMQ_TRACE("...: ", parts[2]);
        auto data = view(parts[2]);
        if (cmd == "SEND") {
            AMQ_TRACE("proxying message");
            return proxy_send(data);
        } else if (cmd == "REPLY") {
            AMQ_TRACE("proxying reply to non-SN incoming message");
            return proxy_reply(data);
        } else if (cmd == "BATCH") {
            AMQ_TRACE("proxy batch jobs");
            auto ptrval = bt_deserialize<uintptr_t>(data);
            return proxy_batch(reinterpret_cast<detail::Batch*>(ptrval));
        } else if (cmd == "INJECT") {
            AMQ_TRACE("proxy inject");
            return proxy_inject_task(detail::deserialize_object<injected_task>(bt_deserialize<uintptr_t>(data)));
        } else if (cmd == "SET_SNS") {
            return proxy_set_active_sns(data);
        } else if (cmd == "UPDATE_SNS") {
            return proxy_update_active_sns(data);
        } else if (cmd == "CONNECT_SN") {
            proxy_connect_sn(data);
            return;
        } else if (cmd == "CONNECT_REMOTE") {
            return proxy_connect_remote(data);
        } else if (cmd == "DISCONNECT") {
            return proxy_disconnect(data);
        } else if (cmd == "TIMER") {
            return proxy_timer(data);
        } else if (cmd == "TIMER_DEL") {
            return proxy_timer_del(bt_deserialize<int>(data));
        }
    } else if (parts.size() == 2) {
        if (cmd == "START") {
            // Command send by the owning thread during startup; we send back a simple READY reply to
            // let it know we are running.
            return route_control(command, route, "READY");
        } else if (cmd == "QUIT") {
            // Asked to quit: set max_workers to zero and tell any idle ones to quit.  We will
            // close workers as they come back to READY status, and then close external
            // connections once all workers are done.
            max_workers = 0;
            for (const auto &route : idle_workers)
                route_control(workers_socket, workers[route].worker_routing_id, "QUIT");
            idle_workers.clear();
            for (auto& [run, busy, queue] : tagged_workers)
              if (!busy)
                route_control(workers_socket, run.worker_routing_id, "QUIT");
            return;
        }
    }
    throw std::runtime_error("ArqmaMQ bug: Proxy received invalid control command: " +
                             std::string{cmd} + " (" + std::to_string(parts.size()) + ")");
}

void ArqmaMQ::proxy_loop() {

#if defined(__linux__) || defined(__sun) || defined(__MINGW32__)
    pthread_setname_np(pthread_self(), "amq-proxy");
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), "amq-proxy");
#elif defined(__MACH__)
    pthread_setname_np("amq-proxy");
#endif

    zap_auth.set(zmq::sockopt::linger, 0);
    zap_auth.bind(ZMQ_ADDR_ZAP);

    workers_socket.set(zmq::sockopt::router_mandatory, true);
    workers_socket.bind(SN_ADDR_WORKERS);

    assert(general_workers > 0);
    if (batch_jobs_reserved < 0)
        batch_jobs_reserved = (general_workers + 1) / 2;
    if (reply_jobs_reserved < 0)
        reply_jobs_reserved = (general_workers + 7) / 8;

    max_workers = general_workers + batch_jobs_reserved + reply_jobs_reserved;
    for (const auto& cat : categories) {
        max_workers += cat.second.reserved_threads;
    }

    if (log_level() >= LogLevel::debug) {
        AMQ_LOG(debug, "Reserving space for ", max_workers, " max workers = ", general_workers, " general plus reservations for:");
        for (const auto& cat : categories)
            AMQ_LOG(debug, "    - ", cat.first, ": ", cat.second.reserved_threads);
        AMQ_LOG(debug, "    - (batch jobs): ", batch_jobs_reserved);
        AMQ_LOG(debug, "    - (reply jobs): ", reply_jobs_reserved);
        AMQ_LOG(debug, "Plus ", tagged_workers.size(), " tagged worker threads");
    }

    workers.reserve(max_workers);
    if (!workers.empty())
        throw std::logic_error("Internal error: proxy thread started with active worker threads");

#ifndef _WIN32
    int saved_umask = -1;
    if (STARTUP_UMASK >= 0)
      saved_umask = umask(STARTUP_UMASK);
#endif

    for (size_t i = 0; i < bind.size(); i++) {
        auto& b = bind[i].second;
        zmq::socket_t listener{context, zmq::socket_type::router};

        std::string auth_domain = bt_serialize(i);
        setup_external_socket(listener);
        listener.set(zmq::sockopt::zap_domain, bt_serialize(i));
        if (b.curve) {
            listener.set(zmq::sockopt::curve_server, true);
            listener.set(zmq::sockopt::curve_publickey, pubkey);
            listener.set(zmq::sockopt::curve_secretkey, privkey);
        }
        listener.set(zmq::sockopt::router_handover, true);
        listener.set(zmq::sockopt::router_mandatory, true);

        listener.bind(bind[i].first);
        AMQ_LOG(info, "ArqmaMQ listening on ", bind[i].first);

        connections.push_back(std::move(listener));
        auto conn_id = next_conn_id++;
        conn_index_to_id.push_back(conn_id);
        incoming_conn_index[conn_id] = connections.size() - 1;
        b.index = connections.size() - 1;
    }

#ifndef _WIN32
    if (saved_umask != -1)
      umask(saved_umask);

    if (SOCKET_GID != -1 or SOCKET_UID != -1)
    {
      for (size_t i = 0; i < bind.size(); i++)
      {
        const address addr(bind[i].first);
        if (addr.ipc())
        {
          if (chown(addr.socket.c_str(), SOCKET_UID, SOCKET_GID) == -1)
          {
            throw std::runtime_error("Cannot set group on " + addr.socket + ": " + strerror(errno));
          }
        }
      }
    }
#endif

    pollitems_stale = true;

    // Also add an internal connection to self so that calling code can avoid needing to
    // special-case rare situations where we are supposed to talk to a quorum member that happens to
    // be ourselves (which can happen, for example, with cross-quoum Blink communication)
    // FIXME: not working
    //listener.bind(SN_ADDR_SELF);

    if (!timers)
        timers.reset(zmq_timers_new());

    auto do_conn_cleanup = [this] { proxy_conn_cleanup(); };
    using CleanupLambda = decltype(do_conn_cleanup);
    if (-1 == zmq_timers_add(timers.get(),
            std::chrono::milliseconds{CONN_CHECK_INTERVAL}.count(),
            // Wrap our lambda into a C function pointer where we pass in the lambda pointer as extra arg
            [](int /*timer_id*/, void* cleanup) { (*static_cast<CleanupLambda*>(cleanup))(); },
            &do_conn_cleanup)) {
        throw zmq::error_t{};
    }

    std::vector<zmq::message_t> parts;

    if (!tagged_workers.empty())
    {
      AMQ_LOG(debug, "Waiting for tagged workers");
      std::unordered_set<std::string_view> waiting_on;
      for (auto& w : tagged_workers)
        waiting_on.emplace(std::get<run_info>(w).worker_routing_id);
      for (; !waiting_on.empty(); parts.clear())
      {
        recv_message_parts(workers_socket, parts);
        if (parts.size() != 2 || view(parts[1]) != "STARTING"sv)
        {
          AMQ_LOG(error, "Received invalid message on worker socket while waiting for tagged thread startup");
          continue;
        }
        AMQ_LOG(debug, "Received STARTING message from ", view(parts[0]));
        if (auto it = waiting_on.find(view(parts[0])); it != waiting_on.end())
          waiting_on.erase(it);
        else
          AMQ_LOG(error, "Received STARTING message from unknown worker ", view(parts[0]));
      }

      for (auto& w : tagged_workers)
      {
        AMQ_LOG(debug, "Telling tagged thread worker ", std::get<run_info>(w).worker_routing_id, " to finish startup");
        route_control(workers_socket, std::get<run_info>(w).worker_routing_id, "START");
      }
    }

    while (true) {
        std::chrono::milliseconds poll_timeout;
        if (max_workers == 0) { // Will be 0 only if we are quitting
            if (std::none_of(workers.begin(), workers.end(), [](auto &w) { return w.worker_thread.joinable(); }) &&
                std::none_of(tagged_workers.begin(), tagged_workers.end(), [](auto &w) { return std::get<0>(w).worker_thread.joinable(); })) {
                // All the workers have finished, so we can finish shutting down
                return proxy_quit();
            }
            poll_timeout = 1s; // We don't keep running timers when we're quitting, so don't have a timer to check
        } else {
            poll_timeout = std::chrono::milliseconds{zmq_timers_timeout(timers.get())};
        }

        if (proxy_skip_one_poll)
            proxy_skip_one_poll = false;
        else {
            AMQ_TRACE("polling for new messages");

            if (pollitems_stale)
                rebuild_pollitems();

            // We poll the control socket and worker socket for any incoming messages.  If we have
            // available worker room then also poll incoming connections and outgoing connections
            // for messages to forward to a worker.  Otherwise, we just look for a control message
            // or a worker coming back with a ready message.
            zmq::poll(pollitems.data(), pollitems.size(), poll_timeout);
        }

        AMQ_TRACE("processing control messages");
        // Retrieve any waiting incoming control messages
        for (parts.clear(); recv_message_parts(command, parts, zmq::recv_flags::dontwait); parts.clear()) {
            proxy_control_message(parts);
        }

        AMQ_TRACE("processing worker messages");
        for (parts.clear(); recv_message_parts(workers_socket, parts, zmq::recv_flags::dontwait); parts.clear()) {
            proxy_worker_message(parts);
        }

        AMQ_TRACE("processing timers");
        zmq_timers_execute(timers.get());

        // Handle any zap authentication
        AMQ_TRACE("processing zap requests");
        process_zap_requests();

        // See if we can drain anything from the current queue before we potentially add to it
        // below.
        AMQ_TRACE("processing queued jobs and messages");
        proxy_process_queue();

        AMQ_TRACE("processing new incoming messages");

        // We round-robin connections when pulling off pending messages one-by-one rather than
        // pulling off all messages from one connection before moving to the next; thus in cases of
        // contention we end up fairly distributing.
        const int num_sockets = connections.size();
        std::queue<int> queue_index;
        for (int i = 0; i < num_sockets; i++)
            queue_index.push(i);

        for (parts.clear(); !queue_index.empty(); parts.clear()) {
            size_t i = queue_index.front();
            queue_index.pop();
            auto& sock = connections[i];

            if (!recv_message_parts(sock, parts, zmq::recv_flags::dontwait))
                continue;

            // We only pull this one message now but then requeue the socket so that after we check
            // all other sockets we come back to this one to check again.
            queue_index.push(i);

            if (parts.empty()) {
                AMQ_LOG(warn, "Ignoring empty (0-part) incoming message");
                continue;
            }

            if (!proxy_handle_builtin(i, parts))
                proxy_to_worker(i, parts);

            if (pollitems_stale) {
                // If our items became stale then we may have just closed a connection and so our
                // queue index maybe also be stale, so restart the proxy loop (so that we rebuild
                // pollitems).
                AMQ_TRACE("pollitems became stale; short-circuiting incoming message loop");
                break;
            }
        }

        AMQ_TRACE("done proxy loop");
    }
}

static bool is_error_response(std::string_view cmd) {
  return cmd == "FORBIDDEN" || cmd == "FORBIDDEN_SN" || cmd == "NOT_A_SERVICE_NODE" || cmd == "UNKNOWNCOMMAND" || cmd == "NO_REPLY_TAG";
}

// Return true if we recognized/handled the builtin command (even if we reject it for whatever
// reason)
bool ArqmaMQ::proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts) {
    size_t incoming = connections[conn_index].get(zmq::sockopt::type) == ZMQ_ROUTER;

    std::string_view route, cmd;
    if (parts.size() < 1 + incoming) {
        AMQ_LOG(warn, "Received empty message; ignoring");
        return true;
    }
    if (incoming) {
        route = view(parts[0]);
        cmd = view(parts[1]);
    } else {
        cmd = view(parts[0]);
    }
    AMQ_TRACE("Checking for builtins: '", cmd, "' from ", peer_address(parts.back()));

    if (cmd == "REPLY") {
        size_t tag_pos = 1 + incoming;
        if (parts.size() <= tag_pos) {
            AMQ_LOG(warn, "Received REPLY without a reply tag; ignoring");
            return true;
        }
        std::string reply_tag{view(parts[tag_pos])};
        auto it = pending_requests.find(reply_tag);
        if (it != pending_requests.end()) {
            AMQ_LOG(debug, "Received REPLY for pending command ", to_hex(reply_tag), "; scheduling callback");
            std::vector<std::string> data;
            data.reserve(parts.size() - (tag_pos + 1));
            for (auto it = parts.begin() + (tag_pos + 1); it != parts.end(); ++it)
                data.emplace_back(view(*it));
            proxy_schedule_reply_job([callback=std::move(it->second.second), data=std::move(data)] {
                    callback(true, std::move(data));
            });
            pending_requests.erase(it);
        } else {
            AMQ_LOG(warn, "Received REPLY with unknown or already handled reply tag (", to_hex(reply_tag), "); ignoring");
        }
        return true;
    } else if (cmd == "HI") {
        if (!incoming) {
            AMQ_LOG(warn, "Got invalid 'HI' message on an outgoing connection; ignoring");
            return true;
        }
        AMQ_LOG(debug, "Incoming client from ", peer_address(parts.back()), " sent HI, replying with HELLO");
        try {
            send_routed_message(connections[conn_index], std::string{route}, "HELLO");
        } catch (const std::exception &e) { AMQ_LOG(warn, "Couldn't reply with HELLO: ", e.what()); }
        return true;
    } else if (cmd == "HELLO") {
        if (incoming) {
            AMQ_LOG(warn, "Got invalid 'HELLO' message on an incoming connection; ignoring");
            return true;
        }
        auto it = std::find_if(pending_connects.begin(), pending_connects.end(),
                [&](auto& pc) { return std::get<size_t>(pc) == conn_index; });
        if (it == pending_connects.end()) {
            AMQ_LOG(warn, "Got invalid 'HELLO' message on an already handshaked incoming connection; ignoring");
            return true;
        }
        auto& pc = *it;
        auto pit = peers.find(std::get<long long>(pc));
        if (pit == peers.end()) {
            AMQ_LOG(warn, "Got invalid 'HELLO' message with invalid conn_id; ignoring");
            return true;
        }

        AMQ_LOG(debug, "Got initial HELLO server response from ", peer_address(parts.back()));
        proxy_schedule_reply_job([on_success=std::move(std::get<ConnectSuccess>(pc)),
                conn=conn_index_to_id[conn_index]] {
            on_success(conn);
        });
        pending_connects.erase(it);
        return true;
    } else if (cmd == "BYE") {
        if (!incoming) {
            AMQ_LOG(debug, "BYE command received. disconnecting from ", peer_address(parts.back()));
            proxy_close_connection(conn_index, 0s);
        } else {
            AMQ_LOG(warn, "Got invalid 'BYE' command on an incoming socket; ignoring");
        }

        return true;
    }
    else if (is_error_response(cmd)) {
        if (parts.size() == (1 + incoming) && cmd == "UNKNOWNCOMMAND") {
            AMQ_LOG(debug, "Received plain UNKNOWNCOMMAND, remote is probably an older arqmad. Ignoring.");
            return true;
        }

        if (parts.size() == (3 + incoming) && view(parts[1 + incoming]) == "REPLY") {
            std::string reply_tag{view(parts[2 + incoming])};
            auto it = pending_requests.find(reply_tag);
            if (it != pending_requests.end()) {
                AMQ_LOG(debug, "Received ", cmd, " REPLY for pending command ", to_hex(reply_tag), "; scheduling failure callback");
                proxy_schedule_reply_job([callback=std::move(it->second.second), cmd=std::string{cmd}] {
                    callback(false, {{std::move(cmd)}});
                });
                pending_requests.erase(it);
            } else {
                AMQ_LOG(warn, "Received REPLY with unknown or already handled reply tag (", to_hex(reply_tag), "); ignoring");
            }
        } else {
            AMQ_LOG(warn, "Received ", cmd, ':', (parts.size() > 1 + incoming ? view(parts[1 + incoming]) : "(unknown command)"sv),
                        " from ", peer_address(parts.back()));
        }
        return true;
    }
    return false;
}

void ArqmaMQ::proxy_process_queue() {
    if (max_workers == 0)
      return;

    for (auto& [run, busy, queue] : tagged_workers)
    {
      if (!busy && !queue.empty())
      {
        busy = true;
        proxy_run_worker(run.load(std::move(queue.front()), false, run.worker_id));
        queue.pop();
      }
    }

    proxy_run_batch_jobs(batch_jobs, batch_jobs_reserved, batch_jobs_active, false);

    // Next any reply batch jobs (which are a bit different from the above, since they are
    // externally triggered but for things we initiated locally).
    proxy_run_batch_jobs(reply_jobs, reply_jobs_reserved, reply_jobs_active, true);

    // Finally general incoming commands
    for (auto it = pending_commands.begin(); it != pending_commands.end() && active_workers() < max_workers; ) {
        auto& pending = *it;
        if (pending.cat.active_threads < pending.cat.reserved_threads
                || active_workers() < general_workers) {
            proxy_run_worker(get_idle_worker().load(std::move(pending)));
            pending.cat.queued--;
            pending.cat.active_threads++;
            assert(pending.cat.queued >= 0);
            it = pending_commands.erase(it);
        } else {
            ++it; // no available general or reserved worker spots for this job right now
        }
    }
}

}
