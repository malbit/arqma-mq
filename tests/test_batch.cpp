#include "arqmamq/batch.h"
#include "common.h"
#include <future>

double do_my_task(int input) {
    if (input % 10 == 7)
        throw std::domain_error("I don't do '7s, sorry");
    if (input == 1)
        return 5.0;
    return 3.0 * input;
}

std::promise<std::pair<double, int>> done;

void continue_big_task(std::vector<arqmamq::job_result<double>> results) {
    double sum = 0;
    int exc_count = 0;
    for (auto& r : results) {
        try {
            sum += r.get();
        } catch (const std::exception& e) {
            exc_count++;
        }
    }
    done.set_value({sum, exc_count});
}

void start_big_task(arqmamq::ArqmaMQ& arqmq) {
    size_t num_jobs = 32;

    arqmamq::Batch<double /*return type*/> batch;
    batch.reserve(num_jobs);
    for (size_t i = 0; i < num_jobs; i++)
        batch.add_job([i]() { return do_my_task(i); });

    batch.completion(&continue_big_task);

    arqmq.batch(std::move(batch));
}


TEST_CASE("batching many small jobs", "[batch-many]") {
    arqmamq::ArqmaMQ arqmq{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    arqmq.set_general_threads(4);
    arqmq.set_batch_threads(4);
    arqmq.start();

    start_big_task(arqmq);
    auto sum = done.get_future().get();
    REQUIRE( sum.first == 1337.0 );
    REQUIRE( sum.second == 3 );
}

TEST_CASE("batch exception propagation", "[batch-exceptions]") {
    arqmamq::ArqmaMQ arqmq{
        "", "", // generate ephemeral keys
        false, // not a service node
        [](auto) { return ""; },
    };
    arqmq.set_general_threads(4);
    arqmq.set_batch_threads(4);
    arqmq.start();

    std::promise<void> done_promise;
    std::future<void> done_future = done_promise.get_future();

    using Catch::Matchers::Message;

    SECTION( "value return" ) {
        arqmamq::Batch<int> batch;
        for (int i : {1, 2})
            batch.add_job([i]() { if (i == 1) return 42; throw std::domain_error("bad value " + std::to_string(i)); });
        batch.completion([&done_promise](auto results) {
                REQUIRE( results.size() == 2 );
                REQUIRE( results[0].get() == 42 );
                REQUIRE_THROWS_MATCHES( results[1].get() == 0, std::domain_error, Message("bad value 2") );
                done_promise.set_value();
                });
        arqmq.batch(std::move(batch));
        done_future.get();
    }

    SECTION( "lvalue return" ) {
        arqmamq::Batch<int&> batch;
        int forty_two = 42;
        for (int i : {1, 2})
            batch.add_job([i,&forty_two]() -> int& {
                    if (i == 1)
                        return forty_two;
                    throw std::domain_error("bad value " + std::to_string(i));
            });
        batch.completion([&done_promise,&forty_two](auto results) {
                REQUIRE( results.size() == 2 );
                auto& r = results[0].get();
                REQUIRE( &r == &forty_two );
                REQUIRE( r == 42 );
                REQUIRE_THROWS_MATCHES( results[1].get(), std::domain_error, Message("bad value 2") );
                done_promise.set_value();
                });
        arqmq.batch(std::move(batch));
        done_future.get();
    }

    SECTION( "void return" ) {
        arqmamq::Batch<void> batch;
        for (int i : {1, 2})
            batch.add_job([i]() { if (i != 1) throw std::domain_error("bad value " + std::to_string(i)); });
        batch.completion([&done_promise](auto results) {
                REQUIRE( results.size() == 2 );
                REQUIRE_NOTHROW( results[0].get() );
                REQUIRE_THROWS_MATCHES( results[1].get(), std::domain_error, Message("bad value 2") );
                done_promise.set_value();
                });
        arqmq.batch(std::move(batch));
        done_future.get();
    }
}
