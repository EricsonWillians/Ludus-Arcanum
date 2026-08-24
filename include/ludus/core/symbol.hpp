#pragma once

#include "ludus/core/id.hpp"

#include <expected>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ludus {

template <typename Id>
class SymbolTable {
  public:
    [[nodiscard]] Id intern(std::string_view name) {
        if (name.empty()) {
            throw std::invalid_argument{"symbol names cannot be empty"};
        }
        if (const auto found = by_name_.find(name); found != by_name_.end()) {
            return found->second;
        }

        const auto next = static_cast<typename Id::value_type>(names_.size() + 1U);
        const Id id{next};
        names_.emplace_back(name);
        by_name_.emplace(names_.back(), id);
        return id;
    }

    [[nodiscard]] std::expected<Id, std::string> find(std::string_view name) const {
        if (const auto found = by_name_.find(name); found != by_name_.end()) {
            return found->second;
        }
        return std::unexpected{"unknown symbol: " + std::string{name}};
    }

    [[nodiscard]] std::expected<std::string_view, std::string> name(Id id) const {
        if (!id.valid() || id.value() > names_.size()) {
            return std::unexpected{"invalid symbol identifier"};
        }
        return names_[id.value() - 1U];
    }

    [[nodiscard]] std::span<const std::string> names() const noexcept { return names_; }
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

    [[nodiscard]] static std::expected<SymbolTable, std::string>
    from_names(std::span<const std::string> names) {
        SymbolTable table;
        for (const auto& name : names) {
            if (name.empty() || table.by_name_.contains(name)) {
                return std::unexpected{"invalid or duplicate serialized symbol"};
            }
            static_cast<void>(table.intern(name));
        }
        return table;
    }

    auto operator<=>(const SymbolTable&) const = default;

  private:
    std::vector<std::string> names_;
    std::map<std::string, Id, std::less<>> by_name_;
};

struct SymbolRegistry {
    SymbolTable<TagId> tags;
    SymbolTable<PropertyId> properties;
    SymbolTable<DirectionId> directions;
    SymbolTable<ActionTypeId> actions;
    SymbolTable<EventTypeId> events;

    auto operator<=>(const SymbolRegistry&) const = default;
};

} // namespace ludus
