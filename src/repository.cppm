module;

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

export module ageof.repository;

export namespace ageof {

struct ApiResponse {
  int status{200};
  std::string body;
};

class Repository final {
 public:
  Repository();
  ~Repository();

  Repository(const Repository&) = delete;
  Repository& operator=(const Repository&) = delete;

  void ping() const;
  ApiResponse dispatch(std::string_view method,
                       std::string_view path,
                       std::string_view body) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ageof