#include "TurnStoreRunner.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

/** What the worker was asked to do. Mirrors TurnStoreResult::Kind. */
struct Job {
    TurnStoreResult::Kind kind = TurnStoreResult::Kind::TurnPublished;
    uint32_t              turnNumber = 0;
    std::string           psid;
    TurnStoreRef          ref;
    std::vector<uint8_t>  payload;
    TurnStoreClient::Config config;
};

}  // namespace

struct TurnStoreRunner::Impl {
    mutable std::mutex      mutex;
    std::condition_variable wake;

    TurnStoreClient::Config config;
    std::deque<Job>         jobs;
    std::deque<TurnStoreResult> results;

    /** Counted rather than derived from `jobs`: one job is off the queue and
     *  in flight while the worker runs it, and a caller asking "is my request
     *  still out" must be told yes for that whole time. */
    size_t   outstanding = 0;
    bool     stopping = false;
    std::thread worker;

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            // A fresh client per job, configured from the snapshot taken when
            // the job was queued. Reconfiguring a shared one from another
            // thread mid-request is the kind of race that shows up once a
            // fortnight and never in a test.
            TurnStoreClient client;
            client.configure(job.config);

            TurnStoreResult result;
            result.kind       = job.kind;
            result.turnNumber = job.turnNumber;
            result.psid       = job.psid;

            switch (job.kind) {
                case TurnStoreResult::Kind::TurnPublished:
                    result.ok = client.publishTurn(job.turnNumber, job.payload,
                                                   result.ref, result.error);
                    break;
                case TurnStoreResult::Kind::OrdersPublished:
                    result.ok = client.publishOrders(job.turnNumber, job.psid,
                                                     job.payload, result.ref,
                                                     result.error);
                    break;
                case TurnStoreResult::Kind::TurnFetched:
                    result.ref = job.ref;
                    result.ok  = client.fetchTurn(job.ref, result.payload, result.error);
                    break;
                case TurnStoreResult::Kind::OrdersFetched:
                    result.ref = job.ref;
                    result.ok  = client.fetchOrders(job.ref, result.payload, result.error);
                    break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                results.push_back(std::move(result));
                if (outstanding > 0) outstanding--;
            }
        }
    }

    void queue(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            job.config = config;
            jobs.push_back(std::move(job));
            outstanding++;
        }
        wake.notify_one();
    }
};

TurnStoreRunner::TurnStoreRunner() : m_impl(new Impl()) {
    m_impl->worker = std::thread([this] { m_impl->run(); });
}

TurnStoreRunner::~TurnStoreRunner() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->wake.notify_all();
    // Joined rather than detached. A detached worker outliving this object
    // would be a thread holding a dangling `this` -- and it happens at exactly
    // the worst moment, when the game is shutting down and nobody is watching.
    if (m_impl->worker.joinable()) m_impl->worker.join();
}

void TurnStoreRunner::configure(const TurnStoreClient::Config& config) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->config = config;
}

TurnStoreClient::Config TurnStoreRunner::config() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->config;
}

bool TurnStoreRunner::automatic() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->config.kind != TurnStoreKind::Manual;
}

void TurnStoreRunner::publishTurn(uint32_t turnNumber,
                                  const std::vector<uint8_t>& bundle) {
    Job job;
    job.kind       = TurnStoreResult::Kind::TurnPublished;
    job.turnNumber = turnNumber;
    job.payload    = bundle;
    m_impl->queue(std::move(job));
}

void TurnStoreRunner::publishOrders(uint32_t turnNumber, const std::string& psid,
                                    const std::vector<uint8_t>& sealed) {
    Job job;
    job.kind       = TurnStoreResult::Kind::OrdersPublished;
    job.turnNumber = turnNumber;
    job.psid       = psid;
    job.payload    = sealed;
    m_impl->queue(std::move(job));
}

void TurnStoreRunner::fetchTurn(uint32_t turnNumber, const TurnStoreRef& ref) {
    Job job;
    job.kind       = TurnStoreResult::Kind::TurnFetched;
    job.turnNumber = turnNumber;
    job.ref        = ref;
    m_impl->queue(std::move(job));
}

void TurnStoreRunner::fetchOrders(uint32_t turnNumber, const std::string& psid,
                                  const TurnStoreRef& ref) {
    Job job;
    job.kind       = TurnStoreResult::Kind::OrdersFetched;
    job.turnNumber = turnNumber;
    job.psid       = psid;
    job.ref        = ref;
    m_impl->queue(std::move(job));
}

bool TurnStoreRunner::nextResult(TurnStoreResult& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->results.empty()) return false;
    out = std::move(m_impl->results.front());
    m_impl->results.pop_front();
    return true;
}

size_t TurnStoreRunner::pending() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->outstanding;
}
