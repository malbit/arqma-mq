#include "common.h"
#include <future>
extern "C" {
#include <sodium.h>
}


TEST_CASE("connections with curve authentication", "[curve][connect]") {
    std::string listen = "tcp://127.0.0.1:4455";
    ArqmaMQ server{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
        get_logger("S» ")
    };
    server.log_level(LogLevel::trace);

    server.listen_curve(listen, [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; });
    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) { m.send_reply("hi"); });
    server.start();

    ArqmaMQ client{get_logger("C» ")};
    client.log_level(LogLevel::trace);

    client.start();

    auto pubkey = server.get_pubkey();
    std::atomic<int> connected{0};
    auto server_conn = client.connect_remote(listen,
            [&](auto conn) { connected = 1; },
            [&](auto conn, string_view reason) { auto lock = catch_lock(); INFO("connection failed: " << reason); },
            pubkey);

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE( i <= 1 );
    REQUIRE( connected.load() );

    bool success = false;
    std::vector<std::string> parts;
    client.request(server_conn, "public.hello", [&](auto success_, auto parts_) { success = success_; parts = parts_; });
    std::this_thread::sleep_for(50ms);
    REQUIRE( success );

}

TEST_CASE("self-connection SN optimization", "[connect][self]") {
    std::string pubkey, privkey;
    pubkey.resize(crypto_box_PUBLICKEYBYTES);
    privkey.resize(crypto_box_SECRETKEYBYTES);
    crypto_box_keypair(reinterpret_cast<unsigned char*>(&pubkey[0]), reinterpret_cast<unsigned char*>(&privkey[0]));
    ArqmaMQ sn{
        pubkey, privkey,
        true,
        [&](auto pk) { if (pk == pubkey) return "tcp://127.0.0.1:5544"; else return ""; },
        get_logger("S» ")
    };

    sn.listen_curve("tcp://127.0.0.1:5544", [&](auto ip, auto pk) { REQUIRE(ip == "127.0.0.1"); return Allow{AuthLevel::none, pk == pubkey}; });
    sn.add_category("a", Access{AuthLevel::none});
    bool invoked = false;
    sn.add_command("a", "b", [&](const Message& m) {
            invoked = true;
            auto lock = catch_lock();
            REQUIRE(m.conn.sn());
            REQUIRE(m.conn.pubkey() == pubkey);
            REQUIRE(!m.data.empty());
            REQUIRE(m.data[0] == "my data");
    });
    sn.log_level(LogLevel::trace);

    sn.start();
    std::this_thread::sleep_for(50ms);
    sn.send(pubkey, "a.b", "my data");
    std::this_thread::sleep_for(50ms);
    REQUIRE(invoked);
}

TEST_CASE("plain-text connections", "[plaintext][connect]") {
    std::string listen = "tcp://127.0.0.1:4455";
    ArqmaMQ server{get_logger("S» ")};
    server.log_level(LogLevel::trace);

    server.add_category("public", Access{AuthLevel::none});
    server.add_request_command("public", "hello", [&](Message& m) { m.send_reply("hi"); });

    server.listen_plain(listen, [](auto /*ip*/, auto /*pk*/) { return Allow{AuthLevel::none, false}; });

    server.start();

    ArqmaMQ client{get_logger("C» ")};
    client.log_level(LogLevel::trace);

    client.start();

    std::atomic<int> connected{0};
    auto c = client.connect_remote(listen,
            [&](auto conn) { connected = 1; },
            [&](auto conn, string_view reason) { auto lock = catch_lock(); INFO("connection failed: " << reason); }
            );

    int i;
    for (i = 0; i < 5; i++) {
        if (connected.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE( i <= 1 );
    REQUIRE( connected.load() );

    bool success = false;
    std::vector<std::string> parts;
    client.request(c, "public.hello", [&](auto success_, auto parts_) { success = success_; parts = parts_; });
    std::this_thread::sleep_for(50ms);
    REQUIRE( success );


}
