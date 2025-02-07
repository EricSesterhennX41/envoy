#include "source/common/common/key_value_store_base.h"

#include "absl/cleanup/cleanup.h"

namespace Envoy {
namespace {

// Removes a length prefixed token from |contents| and returns the token,
// or returns absl::nullopt on failure.
absl::optional<absl::string_view> getToken(absl::string_view& contents, std::string& error) {
  const auto it = contents.find("\n");
  if (it == contents.npos) {
    error = "Bad file: no newline";
    return {};
  }
  uint64_t length;
  if (!absl::SimpleAtoi(contents.substr(0, it), &length)) {
    error = "Bad file: no length";
    return {};
  }
  contents.remove_prefix(it + 1);
  if (contents.size() < length) {
    error = "Bad file: insufficient contents";
    return {};
  }
  absl::string_view token = contents.substr(0, length);
  contents.remove_prefix(length);
  return token;
}

} // namespace

KeyValueStoreBase::KeyValueStoreBase(Event::Dispatcher& dispatcher,
                                     std::chrono::milliseconds flush_interval)
    : flush_timer_(dispatcher.createTimer([this, flush_interval]() {
        flush();
        flush_timer_->enableTimer(flush_interval);
      })) {
  if (flush_interval.count() > 0) {
    flush_timer_->enableTimer(flush_interval);
  }
}

// Assuming |contents| is in the format
// [length]\n[key]\n[length]\n[value]
// parses contents into the provided store.
// This is best effort, and will return false on failure without clearing
// partially parsed data.
bool KeyValueStoreBase::parseContents(absl::string_view contents,
                                      absl::flat_hash_map<std::string, std::string>& store) const {
  std::string error;
  while (!contents.empty()) {
    absl::optional<absl::string_view> key = getToken(contents, error);
    absl::optional<absl::string_view> value;
    if (key.has_value()) {
      value = getToken(contents, error);
    }
    if (!key.has_value() || !value.has_value()) {
      ENVOY_LOG(warn, error);
      return false;
    }
    store.emplace(std::string(key.value()), std::string(value.value()));
  }
  return true;
}

void KeyValueStoreBase::addOrUpdate(absl::string_view key, absl::string_view value) {
  store_.insert_or_assign(key, std::string(value));
  if (!flush_timer_->enabled()) {
    flush();
  }
}

void KeyValueStoreBase::remove(absl::string_view key) {
  store_.erase(key);
  if (!flush_timer_->enabled()) {
    flush();
  }
}

absl::optional<absl::string_view> KeyValueStoreBase::get(absl::string_view key) {
  auto it = store_.find(key);
  if (it == store_.end()) {
    return {};
  }
  return it->second;
}

void KeyValueStoreBase::iterate(ConstIterateCb cb) const {
#ifndef NDEBUG
  // When running in debug mode, verify we don't modify the underlying store
  // while iterating.
  absl::flat_hash_map store_before_iteration = store_;
  absl::Cleanup verify_store_is_not_modified = [this, &store_before_iteration] {
    ASSERT(store_ == store_before_iteration,
           "Expected iterate to not modify the underlying store.");
  };
#endif

  for (const auto& [key, value] : store_) {
    Iterate ret = cb(key, value);
    if (ret == Iterate::Break) {
      return;
    }
  }
}

} // namespace Envoy
