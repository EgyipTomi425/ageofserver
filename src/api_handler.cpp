module;

#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/EventBase.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/json.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/ResponseBuilder.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

module ageof.api_handler;

import ageof.repository;

namespace ageof {
namespace {
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr std::size_t kMaxPathBytes = 2048;

ApiResponse failure(int status, std::string message) {
  return {status, folly::toJson(folly::dynamic::object("error", std::move(message)))};
}

const char* reasonPhrase(int status) noexcept {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

struct Slot final {
  explicit Slot(std::shared_ptr<std::atomic<std::size_t>> count) : count_(std::move(count)) {}
  ~Slot() { count_->fetch_sub(1, std::memory_order_release); }
  std::shared_ptr<std::atomic<std::size_t>> count_;
};

}  // namespace

class WorkPool final {
 public:
  explicit WorkPool(std::size_t threads = 4, std::size_t maxInFlight = 64);

  bool submit(folly::EventBase* eventBase,
              std::function<ApiResponse()> job,
              std::function<void(ApiResponse)> completion);

 private:
  struct State;
  std::shared_ptr<State> state_;
  std::shared_ptr<folly::CPUThreadPoolExecutor> executor_;
};

class ApiHandler final : public proxygen::RequestHandler {
 public:
  ApiHandler(std::shared_ptr<Repository> repository,
             std::shared_ptr<WorkPool> workPool);

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> request) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;
  void onEOM() noexcept override;
  void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;
  void requestComplete() noexcept override;
  void onError(proxygen::ProxygenError error) noexcept override;

 private:
  void send(ApiResponse response) noexcept;
  void destroyIfReady() noexcept;

  std::shared_ptr<Repository> repository_;
  std::shared_ptr<WorkPool> workPool_;
  folly::EventBase* eventBase_{nullptr};
  std::string body_;
  std::string method_;
  std::string path_;
  std::size_t bodySize_{0};
  bool pending_{false};
  bool terminal_{false};
  bool failed_{false};
  bool destroyed_{false};
};

struct WorkPool::State {
  explicit State(std::size_t limit) : outstanding(std::make_shared<std::atomic<std::size_t>>(0)), maxInFlight(limit) {}
  std::shared_ptr<std::atomic<std::size_t>> outstanding;
  const std::size_t maxInFlight;
};

WorkPool::WorkPool(std::size_t threads, std::size_t maxInFlight)
    : state_(std::make_shared<State>(maxInFlight)),
      executor_(std::make_shared<folly::CPUThreadPoolExecutor>(threads)) {}

bool WorkPool::submit(folly::EventBase* eventBase,
                      std::function<ApiResponse()> job,
                      std::function<void(ApiResponse)> completion) {
  auto count = state_->outstanding;
  auto current = count->load(std::memory_order_relaxed);
  while (current < state_->maxInFlight) {
    if (count->compare_exchange_weak(current, current + 1,
                                     std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) {
      auto slot = std::make_shared<Slot>(count);
      try {
        executor_->add([eventBase, slot, job = std::move(job),
                        completion = std::move(completion)]() mutable {
          ApiResponse response;
          try {
            response = job();
          } catch (const std::invalid_argument& ex) {
            response = failure(400, ex.what());
          } catch (const std::exception&) {
            response = failure(500, "Request failed");
          } catch (...) {
            response = failure(500, "Request failed");
          }
          eventBase->runInEventBaseThread(
              [slot = std::move(slot), completion = std::move(completion),
               response = std::move(response)]() mutable {
                completion(std::move(response));
              });
        });
        return true;
      } catch (...) {
        return false;
      }
    }
  }
  return false;
}

ApiHandler::ApiHandler(std::shared_ptr<Repository> repository,
                       std::shared_ptr<WorkPool> workPool)
    : repository_(std::move(repository)), workPool_(std::move(workPool)) {}

void ApiHandler::onRequest(std::unique_ptr<proxygen::HTTPMessage> request) noexcept {
  eventBase_ = folly::EventBaseManager::get()->getEventBase();
  method_ = request->getMethodString();
  path_ = request->getPath();
  if (const auto query = path_.find('?'); query != std::string::npos) {
    path_.resize(query);
  }
  if (path_.size() > kMaxPathBytes) {
    send(failure(414, "Path too long"));
    return;
  }
}

void ApiHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
  if (!body || failed_) {
    return;
  }
  const auto length = body->computeChainDataLength();
  if (length > kMaxBodyBytes - std::min(bodySize_, kMaxBodyBytes)) {
    failed_ = true;
    send(failure(413, "Request body too large"));
    return;
  }
  bodySize_ += length;
  try {
    body->coalesce();
    body_.append(reinterpret_cast<const char*>(body->data()), body->length());
  } catch (...) {
    failed_ = true;
    downstream_->sendAbort();
  }
}

void ApiHandler::onEOM() noexcept {
  if (failed_ || terminal_) {
    return;
  }
  if (eventBase_ == nullptr) {
    send(failure(500, "Event loop unavailable"));
    return;
  }

  std::string body;
  try {
    body = std::move(body_);
  } catch (...) {
    send(failure(400, "Invalid request body"));
    return;
  }

  const auto repository = repository_;
  const auto method = method_;
  const auto path = path_;
  pending_ = true;
  if (!workPool_->submit(eventBase_,
                         [repository, method, path, body = std::move(body)] {
                           return repository->dispatch(method, path, body);
                         },
                         [this](ApiResponse response) {
                           if (!terminal_ && !failed_) {
                             send(std::move(response));
                           }
                           pending_ = false;
                           destroyIfReady();
                         })) {
    pending_ = false;
    send(failure(503, "Database workers are busy"));
  }
}

void ApiHandler::onUpgrade(proxygen::UpgradeProtocol) noexcept {
  failed_ = true;
}

void ApiHandler::requestComplete() noexcept {
  terminal_ = true;
  destroyIfReady();
}

void ApiHandler::onError(proxygen::ProxygenError) noexcept {
  failed_ = true;
  terminal_ = true;
  destroyIfReady();
}

void ApiHandler::send(ApiResponse response) noexcept {
  if (terminal_ || downstream_ == nullptr) {
    return;
  }
  try {
    proxygen::ResponseBuilder(downstream_)
        .status(response.status, reasonPhrase(response.status))
        .header("Content-Type", "application/json; charset=utf-8")
        .header("Cache-Control", "no-store")
      .body(response.body)
        .sendWithEOM();
  } catch (...) {
    failed_ = true;
    downstream_->sendAbort();
  }
}

void ApiHandler::destroyIfReady() noexcept {
  if (terminal_ && !pending_ && !destroyed_) {
    destroyed_ = true;
    delete this;
  }
}

namespace {

class ApiHandlerFactory final : public proxygen::RequestHandlerFactory {
 public:
  ApiHandlerFactory(std::shared_ptr<Repository> repository,
                    std::shared_ptr<WorkPool> workPool)
      : repository_(std::move(repository)), workPool_(std::move(workPool)) {}

  void onServerStart(folly::EventBase*) noexcept override {}
  void onServerStop() noexcept override {}

  proxygen::RequestHandler* onRequest(proxygen::RequestHandler*,
                                      proxygen::HTTPMessage*) noexcept override {
    return new ApiHandler(repository_, workPool_);
  }

 private:
  std::shared_ptr<Repository> repository_;
  std::shared_ptr<WorkPool> workPool_;
};

}  // namespace

int runApiServer() {
  auto repository = std::make_shared<Repository>();
  repository->ping();

  const auto threads = std::max(1u, std::thread::hardware_concurrency());
  auto workPool = std::make_shared<WorkPool>(4, 64);
  proxygen::HTTPServerOptions options;
  options.threads = threads;
  options.idleTimeout = std::chrono::seconds(60);
  options.shutdownOn = {SIGINT, SIGTERM};
  options.enableContentCompression = false;
  options.handlerFactories = proxygen::RequestHandlerChain()
      .addThen<ApiHandlerFactory>(repository, workPool)
      .build();

  proxygen::HTTPServer server(std::move(options));
  server.bind({
      {folly::SocketAddress("0.0.0.0", 18432, true),
       proxygen::HTTPServer::Protocol::HTTP2},
  });
  server.start();
  return EXIT_SUCCESS;
}

}  // namespace ageof
