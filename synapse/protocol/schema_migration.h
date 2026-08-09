// ============================================================================
// synapse/protocol/schema_migration.h
// Project Synapse – Schema Versioning & Migration System
//
// All data formats are versioned. Migrations are registered as functions
// that transform data from version N to version N+1. The system auto-discovers
// migration paths via BFS. Backward compatibility: readers can always read
// data 1 version older.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace synapse::protocol {

// Migration function: transforms data from one version to the next
using MigrationFunc = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)>;

// A single migration step
struct MigrationStep {
    uint32_t from_version;
    uint32_t to_version;
    MigrationFunc migrate;
};

// Versioned message header (prepended to all serialized data)
struct MessageHeader {
    uint32_t magic{0x53594E41};  // "SYNA"
    uint32_t schema_version{0};
    uint32_t payload_size{0};
    uint32_t checksum{0};
};

// Schema migration registry and executor
class SchemaMigration {
public:
    // Register a migration from version N to N+1
    void register_migration(uint32_t from, uint32_t to, MigrationFunc func) {
        steps_[from * 1000 + to] = {from, to, std::move(func)};
    }

    // Migrate data from one version to another (auto-discovers path)
    std::vector<uint8_t> migrate(const std::vector<uint8_t>& data,
                                 uint32_t from_ver, uint32_t to_ver) {
        if (from_ver == to_ver) return data;

        // Find migration path via BFS
        auto path = find_path(from_ver, to_ver);
        if (path.empty()) {
            throw std::runtime_error("No migration path from v" +
                std::to_string(from_ver) + " to v" + std::to_string(to_ver));
        }

        // Apply migrations along the path
        auto current = data;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            auto* step = find_step(path[i], path[i + 1]);
            if (!step) {
                throw std::runtime_error("Missing migration step v" +
                    std::to_string(path[i]) + " → v" + std::to_string(path[i + 1]));
            }
            current = step->migrate(current);
        }

        return current;
    }

    // Check if a migration path exists
    bool has_path(uint32_t from, uint32_t to) const {
        return !find_path(from, to).empty();
    }

    // Backward compatibility: can reader_ver read data_ver?
    // Reader can read data up to 1 version older
    bool is_compatible(uint32_t reader_ver, uint32_t data_ver) const {
        if (data_ver == 0) return reader_ver == 0;  // Only v0 reader reads v0 data
        return reader_ver + 1 >= data_ver;
    }

    // Get latest registered version
    uint32_t latest_version() const {
        uint32_t max_ver = 0;
        for (const auto& [key, step] : steps_) {
            if (step.to_version > max_ver) max_ver = step.to_version;
        }
        return max_ver;
    }

    // Number of registered migrations
    size_t migration_count() const { return steps_.size(); }

private:
    // Key: from*1000 + to
    std::unordered_map<uint32_t, MigrationStep> steps_;

    const MigrationStep* find_step(uint32_t from, uint32_t to) const {
        auto it = steps_.find(from * 1000 + to);
        return it != steps_.end() ? &it->second : nullptr;
    }

    // BFS to find shortest migration path
    std::vector<uint32_t> find_path(uint32_t from, uint32_t to) const {
        if (from == to) return {from};

        // Build adjacency list
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        for (const auto& [key, step] : steps_) {
            adj[step.from_version].push_back(step.to_version);
        }

        // BFS
        std::queue<uint32_t> q;
        std::unordered_map<uint32_t, uint32_t> parent;
        q.push(from);
        parent[from] = from;

        while (!q.empty()) {
            auto current = q.front(); q.pop();

            if (current == to) {
                // Reconstruct path
                std::vector<uint32_t> path;
                uint32_t node = to;
                while (node != from) {
                    path.push_back(node);
                    node = parent.at(node);
                }
                path.push_back(from);
                std::reverse(path.begin(), path.end());
                return path;
            }

            auto it = adj.find(current);
            if (it != adj.end()) {
                for (uint32_t neighbor : it->second) {
                    if (parent.find(neighbor) == parent.end()) {
                        parent[neighbor] = current;
                        q.push(neighbor);
                    }
                }
            }
        }

        return {};  // No path found
    }
};

}  // namespace synapse::protocol
